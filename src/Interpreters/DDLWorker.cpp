#include <Interpreters/DDLWorker.h>
#include <Interpreters/DDLTask.h>
#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTOptimizeQuery.h>
#include <Parsers/ASTQueryWithOnCluster.h>
#include <Parsers/ASTQueryWithTableAndOutput.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <Parsers/queryToString.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadBufferFromString.h>
#include <Storages/IStorage.h>
#include <Interpreters/executeQuery.h>
#include <Interpreters/Cluster.h>
#include <Interpreters/Context.h>
#include <Common/setThreadName.h>
#include <Common/randomSeed.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/isLocalAddress.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Poco/Timestamp.h>
#include <common/sleep.h>
#include <common/getFQDNOrHostName.h>
#include <common/logger_useful.h>
#include <random>
#include <pcg_random.hpp>


namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
    extern const int TIMEOUT_EXCEEDED;
    extern const int UNFINISHED;
}


namespace
{

/** Caveats: usage of locks in ZooKeeper is incorrect in 99% of cases,
  *  and highlights your poor understanding of distributed systems.
  *
  * It's only correct if all the operations that are performed under lock
  *  are atomically checking that the lock still holds
  *  or if we ensure that these operations will be undone if lock is lost
  *  (due to ZooKeeper session loss) that's very difficult to achieve.
  *
  * It's Ok if every operation that we perform under lock is actually operation in ZooKeeper.
  *
  * In 1% of cases when you can correctly use Lock, the logic is complex enough, so you don't need this class.
  *
  * TLDR: Don't use this code.
  * We only have a few cases of it's usage and it will be removed.
  */
class ZooKeeperLock
{
public:
    /// lock_prefix - path where the ephemeral lock node will be created
    /// lock_name - the name of the ephemeral lock node
    ZooKeeperLock(
        const zkutil::ZooKeeperPtr & zookeeper_,
        const std::string & lock_prefix_,
        const std::string & lock_name_,
        const std::string & lock_message_ = "")
    :
        zookeeper(zookeeper_),
        lock_path(lock_prefix_ + "/" + lock_name_),
        lock_message(lock_message_),
        log(&Poco::Logger::get("zkutil::Lock"))
    {
        zookeeper->createIfNotExists(lock_prefix_, "");
    }

    ~ZooKeeperLock()
    {
        try
        {
            unlock();
        }
        catch (...)
        {
            DB::tryLogCurrentException(__PRETTY_FUNCTION__);
        }
    }

    void unlock()
    {
        Coordination::Stat stat;
        std::string dummy;
        bool result = zookeeper->tryGet(lock_path, dummy, &stat);

        if (result && stat.ephemeralOwner == zookeeper->getClientID())
            zookeeper->remove(lock_path, -1);
        else
            LOG_WARNING(log, "Lock is lost. It is normal if session was expired. Path: {}/{}", lock_path, lock_message);
    }

    bool tryLock()
    {
        std::string dummy;
        Coordination::Error code = zookeeper->tryCreate(lock_path, lock_message, zkutil::CreateMode::Ephemeral, dummy);

        if (code == Coordination::Error::ZNODEEXISTS)
        {
            return false;
        }
        else if (code == Coordination::Error::ZOK)
        {
            return true;
        }
        else
        {
            throw Coordination::Exception(code);
        }
    }

private:
    zkutil::ZooKeeperPtr zookeeper;

    std::string lock_path;
    std::string lock_message;
    Poco::Logger * log;

};

std::unique_ptr<ZooKeeperLock> createSimpleZooKeeperLock(
    const zkutil::ZooKeeperPtr & zookeeper, const String & lock_prefix, const String & lock_name, const String & lock_message)
{
    return std::make_unique<ZooKeeperLock>(zookeeper, lock_prefix, lock_name, lock_message);
}

}


DDLWorker::DDLWorker(int pool_size_, const std::string & zk_root_dir, const Context & context_, const Poco::Util::AbstractConfiguration * config, const String & prefix,
                     const String & logger_name)
    : context(context_)
    , log(&Poco::Logger::get(logger_name))
    , pool_size(pool_size_)
{
    if (1 < pool_size)
    {
        LOG_WARNING(log, "DDLWorker is configured to use multiple threads. "
                         "It's not recommended because queries can be reordered. Also it may cause some unknown issues to appear.");
        worker_pool.emplace(pool_size);
    }
    queue_dir = zk_root_dir;
    if (queue_dir.back() == '/')
        queue_dir.resize(queue_dir.size() - 1);

    if (config)
    {
        task_max_lifetime = config->getUInt64(prefix + ".task_max_lifetime", static_cast<UInt64>(task_max_lifetime));
        cleanup_delay_period = config->getUInt64(prefix + ".cleanup_delay_period", static_cast<UInt64>(cleanup_delay_period));
        max_tasks_in_queue = std::max<UInt64>(1, config->getUInt64(prefix + ".max_tasks_in_queue", max_tasks_in_queue));

        if (config->has(prefix + ".profile"))
            context.setSetting("profile", config->getString(prefix + ".profile"));
    }

    if (context.getSettingsRef().readonly)
    {
        LOG_WARNING(log, "Distributed DDL worker is run with readonly settings, it will not be able to execute DDL queries Set appropriate system_profile or distributed_ddl.profile to fix this.");
    }

    host_fqdn = getFQDNOrHostName();
    host_fqdn_id = Cluster::Address::toString(host_fqdn, context.getTCPPort());
}

void DDLWorker::startup()
{
    main_thread = ThreadFromGlobalPool(&DDLWorker::runMainThread, this);
    cleanup_thread = ThreadFromGlobalPool(&DDLWorker::runCleanupThread, this);
}

void DDLWorker::shutdown()
{
    stop_flag = true;
    queue_updated_event->set();
    cleanup_event->set();
}

DDLWorker::~DDLWorker()
{
    shutdown();
    if (worker_pool)
        worker_pool->wait();
    if (main_thread.joinable())
        main_thread.join();
    if (cleanup_thread.joinable())
        cleanup_thread.join();
}


ZooKeeperPtr DDLWorker::tryGetZooKeeper() const
{
    std::lock_guard lock(zookeeper_mutex);
    return current_zookeeper;
}

ZooKeeperPtr DDLWorker::getAndSetZooKeeper()
{
    std::lock_guard lock(zookeeper_mutex);

    if (!current_zookeeper || current_zookeeper->expired())
        current_zookeeper = context.getZooKeeper();

    return current_zookeeper;
}


DDLTaskPtr DDLWorker::initAndCheckTask(const String & entry_name, String & out_reason, const ZooKeeperPtr & zookeeper)
{
    String node_data;
    String entry_path = queue_dir + "/" + entry_name;

    auto task = std::make_unique<DDLTask>(entry_name, entry_path);

    if (!zookeeper->tryGet(entry_path, node_data))
    {
        /// It is Ok that node could be deleted just now. It means that there are no current host in node's host list.
        out_reason = "The task was deleted";
        return {};
    }

    auto write_error_status = [&](const String & host_id, const String & error_message, const String & reason)
    {
        LOG_ERROR(log, "Cannot parse DDL task {}: {}. Will try to send error status: {}", entry_name, reason, error_message);
        createStatusDirs(entry_path, zookeeper);
        zookeeper->tryCreate(entry_path + "/finished/" + host_id, error_message, zkutil::CreateMode::Persistent);
    };

    try
    {
        /// Stage 1: parse entry
        task->entry.parse(node_data);
    }
    catch (...)
    {
        /// What should we do if we even cannot parse host name and therefore cannot properly submit execution status?
        /// We can try to create fail node using FQDN if it equal to host name in cluster config attempt will be successful.
        /// Otherwise, that node will be ignored by DDLQueryStatusInputStream.
        out_reason = "Incorrect task format";
        write_error_status(host_fqdn_id, ExecutionStatus::fromCurrentException().serializeText(), out_reason);
        return {};
    }

    /// Stage 2: resolve host_id and check if we should execute query or not
    if (!task->findCurrentHostID(context, log))
    {
        out_reason = "There is no a local address in host list";
        return {};
    }

    try
    {
        /// Stage 3.1: parse query
        task->parseQueryFromEntry(context);
        /// Stage 3.2: check cluster and find the host in cluster
        task->setClusterInfo(context, log);
    }
    catch (...)
    {
        out_reason = "Cannot parse query or obtain cluster info";
        write_error_status(task->host_id_str, ExecutionStatus::fromCurrentException().serializeText(), out_reason);
        return {};
    }

    if (zookeeper->exists(task->getFinishedNodePath()))
    {
        out_reason = "Task has been already processed";
        return {};
    }

    /// Now task is ready for execution
    return task;
}


static void filterAndSortQueueNodes(Strings & all_nodes)
{
    all_nodes.erase(std::remove_if(all_nodes.begin(), all_nodes.end(), [] (const String & s) { return !startsWith(s, "query-"); }), all_nodes.end());
    std::sort(all_nodes.begin(), all_nodes.end());
}

void DDLWorker::scheduleTasks()
{
    LOG_DEBUG(log, "Scheduling tasks");
    auto zookeeper = tryGetZooKeeper();

    Strings queue_nodes = zookeeper->getChildren(queue_dir, nullptr, queue_updated_event);
    filterAndSortQueueNodes(queue_nodes);
    if (queue_nodes.empty())
    {
        LOG_TRACE(log, "No tasks to schedule");
        return;
    }

    bool server_startup = current_tasks.empty();

    auto begin_node = server_startup
        ? queue_nodes.begin()
        : std::upper_bound(queue_nodes.begin(), queue_nodes.end(), current_tasks.back()->entry_name);

    for (auto it = begin_node; it != queue_nodes.end() && !stop_flag; ++it)
    {
        String entry_name = *it;
        LOG_TRACE(log, "Checking task {}", entry_name);

        String reason;
        auto task = initAndCheckTask(entry_name, reason, zookeeper);
        if (!task)
        {
            LOG_DEBUG(log, "Will not execute task {}: {}", entry_name, reason);
            task->was_executed = true;
            saveTask(std::move(task)); //FIXME questionable
            continue;
        }

        auto & saved_task = saveTask(std::move(task));

        if (worker_pool)
        {
            worker_pool->scheduleOrThrowOnError([this, &saved_task]()
            {
                setThreadName("DDLWorkerExec");
                processTask(saved_task);
            });
        }
        else
        {
            processTask(saved_task);
        }
    }
}

DDLTaskBase & DDLWorker::saveTask(DDLTaskPtr && task)
{
    if (current_tasks.size() == pool_size)
    {
        assert(current_tasks.front()->was_executed);
        current_tasks.pop_front();
    }
    current_tasks.emplace_back(std::move(task));
    return *current_tasks.back();
}

bool DDLWorker::tryExecuteQuery(const String & query, const DDLTaskBase & task, ExecutionStatus & status)
{
    /// Add special comment at the start of query to easily identify DDL-produced queries in query_log
    String query_prefix = "/* ddl_entry=" + task.entry_name + " */ ";
    String query_to_execute = query_prefix + query;

    ReadBufferFromString istr(query_to_execute);
    String dummy_string;
    WriteBufferFromString ostr(dummy_string);

    try
    {
        auto query_context = task.makeQueryContext(context);
        executeQuery(istr, ostr, false, *query_context, {});
    }
    catch (...)
    {
        status = ExecutionStatus::fromCurrentException();
        tryLogCurrentException(log, "Query " + query + " wasn't finished successfully");

        return false;
    }

    status = ExecutionStatus(0);
    LOG_DEBUG(log, "Executed query: {}", query);

    return true;
}

void DDLWorker::attachToThreadGroup()
{
    if (thread_group)
    {
        /// Put all threads to one thread pool
        CurrentThread::attachToIfDetached(thread_group);
    }
    else
    {
        CurrentThread::initializeQuery();
        thread_group = CurrentThread::getGroup();
    }
}

void DDLWorker::processTask(DDLTaskBase & task)
{
    auto zookeeper = tryGetZooKeeper();

    LOG_DEBUG(log, "Processing task {} ({})", task.entry_name, task.entry.query);

    String active_node_path = task.getActiveNodePath();
    String finished_node_path = task.getFinishedNodePath();

    String dummy;
    auto code = zookeeper->tryCreate(active_node_path, "", zkutil::CreateMode::Ephemeral, dummy);

    if (code == Coordination::Error::ZNONODE)
    {
        /// There is no parent
        createStatusDirs(task.entry_path, zookeeper);
        zookeeper->create(active_node_path, "", zkutil::CreateMode::Ephemeral);
    }
    else
        throw Coordination::Exception(code, active_node_path);

    if (!task.was_executed)     // FIXME always true
    {
        try
        {
            String rewritten_query = queryToString(task.query);
            LOG_DEBUG(log, "Executing query: {}", rewritten_query);

            StoragePtr storage;
            if (auto * query_with_table = dynamic_cast<ASTQueryWithTableAndOutput *>(task.query.get()); query_with_table)
            {
                if (!query_with_table->table.empty())
                {
                    /// It's not CREATE DATABASE
                    auto table_id = context.tryResolveStorageID(*query_with_table, Context::ResolveOrdinary);
                    storage = DatabaseCatalog::instance().tryGetTable(table_id, context);
                }

                task.execute_on_leader = storage && taskShouldBeExecutedOnLeader(task.query, storage) && !task.is_circular_replicated;
            }

            if (task.execute_on_leader)
                tryExecuteQueryOnLeaderReplica(task, storage, rewritten_query, task.entry_path, zookeeper);
            else
                tryExecuteQuery(rewritten_query, task, task.execution_status);
        }
        catch (const Coordination::Exception &)
        {
            throw;
        }
        catch (...)
        {
            tryLogCurrentException(log, "An error occurred before execution of DDL task: ");
            task.execution_status = ExecutionStatus::fromCurrentException("An error occurred before execution");
        }

        /// We need to distinguish ZK errors occurred before and after query executing
        task.was_executed = true;
    }

    /// FIXME: if server fails right here, the task will be executed twice. We need WAL here.
    /// Another possible issue: if ZooKeeper session is lost here, we will recover connection and execute the task second time.



    /// Delete active flag and create finish flag
    Coordination::Requests ops;
    ops.emplace_back(zkutil::makeRemoveRequest(active_node_path, -1));
    ops.emplace_back(zkutil::makeCreateRequest(finished_node_path, task.execution_status.serializeText(), zkutil::CreateMode::Persistent));

    //FIXME replace with multi(...) or use MetadataTransaction
    Coordination::Responses responses;
    auto res = zookeeper->tryMulti(ops, responses);
    if (res != Coordination::Error::ZNODEEXISTS && res != Coordination::Error::ZNONODE)
        zkutil::KeeperMultiException::check(res, ops, responses);
}


bool DDLWorker::taskShouldBeExecutedOnLeader(const ASTPtr ast_ddl, const StoragePtr storage)
{
    /// Pure DROP queries have to be executed on each node separately
    if (auto * query = ast_ddl->as<ASTDropQuery>(); query && query->kind != ASTDropQuery::Kind::Truncate)
        return false;

    if (!ast_ddl->as<ASTAlterQuery>() && !ast_ddl->as<ASTOptimizeQuery>() && !ast_ddl->as<ASTDropQuery>())
        return false;

    return storage->supportsReplication();
}

bool DDLWorker::tryExecuteQueryOnLeaderReplica(
    DDLTaskBase & task,
    StoragePtr storage,
    const String & rewritten_query,
    const String & /*node_path*/,
    const ZooKeeperPtr & zookeeper)
{
    StorageReplicatedMergeTree * replicated_storage = dynamic_cast<StorageReplicatedMergeTree *>(storage.get());

    /// If we will develop new replicated storage
    if (!replicated_storage)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Storage type '{}' is not supported by distributed DDL", storage->getName());

    String shard_path = task.getShardNodePath();
    String is_executed_path = shard_path + "/executed";
    String tries_to_execute_path = shard_path + "/tries_to_execute";
    zookeeper->createAncestors(shard_path + "/");

    /// Node exists, or we will create or we will get an exception
    zookeeper->tryCreate(tries_to_execute_path, "0", zkutil::CreateMode::Persistent);

    static constexpr int MAX_TRIES_TO_EXECUTE = 3;
    static constexpr int MAX_EXECUTION_TIMEOUT_SEC = 3600;

    String executed_by;

    zkutil::EventPtr event = std::make_shared<Poco::Event>();
    /// We must use exists request instead of get, because zookeeper will not setup event
    /// for non existing node after get request
    if (zookeeper->exists(is_executed_path, nullptr, event))
    {
        LOG_DEBUG(log, "Task {} has already been executed by replica ({}) of the same shard.", task.entry_name, zookeeper->get(is_executed_path));
        return true;
    }

    pcg64 rng(randomSeed());

    auto lock = createSimpleZooKeeperLock(zookeeper, shard_path, "lock", task.host_id_str);

    Stopwatch stopwatch;

    bool executed_by_leader = false;
    /// Defensive programming. One hour is more than enough to execute almost all DDL queries.
    /// If it will be very long query like ALTER DELETE for a huge table it's still will be executed,
    /// but DDL worker can continue processing other queries.
    while (stopwatch.elapsedSeconds() <= MAX_EXECUTION_TIMEOUT_SEC)
    {
        StorageReplicatedMergeTree::Status status;
        replicated_storage->getStatus(status);

        /// Any replica which is leader tries to take lock
        if (status.is_leader && lock->tryLock())
        {
            /// In replicated merge tree we can have multiple leaders. So we can
            /// be "leader" and took lock, but another "leader" replica may have
            /// already executed this task.
            if (zookeeper->tryGet(is_executed_path, executed_by))
            {
                LOG_DEBUG(log, "Task {} has already been executed by replica ({}) of the same shard.", task.entry_name, executed_by);
                executed_by_leader = true;
                break;
            }

            /// Checking and incrementing counter exclusively.
            size_t counter = parse<int>(zookeeper->get(tries_to_execute_path));
            if (counter > MAX_TRIES_TO_EXECUTE)
                break;

            zookeeper->set(tries_to_execute_path, toString(counter + 1));

            /// If the leader will unexpectedly changed this method will return false
            /// and on the next iteration new leader will take lock
            if (tryExecuteQuery(rewritten_query, task, task.execution_status))
            {
                //FIXME replace with create(...) or remove and use MetadataTransaction
                zookeeper->createIfNotExists(is_executed_path, task.host_id_str);
                executed_by_leader = true;
                break;
            }

            lock->unlock();
        }

        /// Waiting for someone who will execute query and change is_executed_path node
        if (event->tryWait(std::uniform_int_distribution<int>(0, 1000)(rng)))
        {
            LOG_DEBUG(log, "Task {} has already been executed by replica ({}) of the same shard.", task.entry_name, zookeeper->get(is_executed_path));
            executed_by_leader = true;
            break;
        }
        else
        {
            String tries_count;
            zookeeper->tryGet(tries_to_execute_path, tries_count);
            if (parse<int>(tries_count) > MAX_TRIES_TO_EXECUTE)
            {
                /// Nobody will try to execute query again
                LOG_WARNING(log, "Maximum retries count for task {} exceeded, cannot execute replicated DDL query", task.entry_name);
                break;
            }
            else
            {
                /// Will try to wait or execute
                LOG_TRACE(log, "Task {} still not executed, will try to wait for it or execute ourselves, tries count {}", task.entry_name, tries_count);
            }
        }
    }

    /// Not executed by leader so was not executed at all
    if (!executed_by_leader)
    {
        /// If we failed with timeout
        if (stopwatch.elapsedSeconds() >= MAX_EXECUTION_TIMEOUT_SEC)
        {
            LOG_WARNING(log, "Task {} was not executed by anyone, maximum timeout {} seconds exceeded", task.entry_name, MAX_EXECUTION_TIMEOUT_SEC);
            task.execution_status = ExecutionStatus(ErrorCodes::TIMEOUT_EXCEEDED, "Cannot execute replicated DDL query, timeout exceeded");
        }
        else /// If we exceeded amount of tries
        {
            LOG_WARNING(log, "Task {} was not executed by anyone, maximum number of retries exceeded", task.entry_name);
            task.execution_status = ExecutionStatus(ErrorCodes::UNFINISHED, "Cannot execute replicated DDL query, maximum retires exceeded");
        }
        return false;
    }

    LOG_DEBUG(log, "Task {} has already been executed by replica ({}) of the same shard.", task.entry_name, zookeeper->get(is_executed_path));
    return true;
}


void DDLWorker::cleanupQueue(Int64 current_time_seconds, const ZooKeeperPtr & zookeeper)
{
    LOG_DEBUG(log, "Cleaning queue");

    Strings queue_nodes = zookeeper->getChildren(queue_dir);
    filterAndSortQueueNodes(queue_nodes);

    size_t num_outdated_nodes = (queue_nodes.size() > max_tasks_in_queue) ? queue_nodes.size() - max_tasks_in_queue : 0;
    auto first_non_outdated_node = queue_nodes.begin() + num_outdated_nodes;

    for (auto it = queue_nodes.cbegin(); it < queue_nodes.cend(); ++it)
    {
        if (stop_flag)
            return;

        String node_name = *it;
        String node_path = queue_dir + "/" + node_name;
        String lock_path = node_path + "/lock";

        Coordination::Stat stat;
        String dummy;

        try
        {
            /// Already deleted
            if (!zookeeper->exists(node_path, &stat))
                continue;

            /// Delete node if its lifetime is expired (according to task_max_lifetime parameter)
            constexpr UInt64 zookeeper_time_resolution = 1000;
            Int64 zookeeper_time_seconds = stat.ctime / zookeeper_time_resolution;
            bool node_lifetime_is_expired = zookeeper_time_seconds + task_max_lifetime < current_time_seconds;

            /// If too many nodes in task queue (> max_tasks_in_queue), delete oldest one
            bool node_is_outside_max_window = it < first_non_outdated_node;

            if (!node_lifetime_is_expired && !node_is_outside_max_window)
                continue;

            /// Skip if there are active nodes (it is weak guard)
            if (zookeeper->exists(node_path + "/active", &stat) && stat.numChildren > 0)
            {
                LOG_INFO(log, "Task {} should be deleted, but there are active workers. Skipping it.", node_name);
                continue;
            }

            /// Usage of the lock is not necessary now (tryRemoveRecursive correctly removes node in a presence of concurrent cleaners)
            /// But the lock will be required to implement system.distributed_ddl_queue table
            auto lock = createSimpleZooKeeperLock(zookeeper, node_path, "lock", host_fqdn_id);
            if (!lock->tryLock())
            {
                LOG_INFO(log, "Task {} should be deleted, but it is locked. Skipping it.", node_name);
                continue;
            }

            if (node_lifetime_is_expired)
                LOG_INFO(log, "Lifetime of task {} is expired, deleting it", node_name);
            else if (node_is_outside_max_window)
                LOG_INFO(log, "Task {} is outdated, deleting it", node_name);

            /// Deleting
            {
                Strings children = zookeeper->getChildren(node_path);
                for (const String & child : children)
                {
                    if (child != "lock")
                        zookeeper->tryRemoveRecursive(node_path + "/" + child);
                }

                /// Remove the lock node and its parent atomically
                Coordination::Requests ops;
                ops.emplace_back(zkutil::makeRemoveRequest(lock_path, -1));
                ops.emplace_back(zkutil::makeRemoveRequest(node_path, -1));
                zookeeper->multi(ops);
            }
        }
        catch (...)
        {
            LOG_INFO(log, "An error occurred while checking and cleaning task {} from queue: {}", node_name, getCurrentExceptionMessage(false));
        }
    }
}


/// Try to create nonexisting "status" dirs for a node
void DDLWorker::createStatusDirs(const std::string & node_path, const ZooKeeperPtr & zookeeper)
{
    Coordination::Requests ops;
    {
        Coordination::CreateRequest request;
        request.path = node_path + "/active";
        ops.emplace_back(std::make_shared<Coordination::CreateRequest>(std::move(request)));
    }
    {
        Coordination::CreateRequest request;
        request.path = node_path + "/finished";
        ops.emplace_back(std::make_shared<Coordination::CreateRequest>(std::move(request)));
    }
    Coordination::Responses responses;
    Coordination::Error code = zookeeper->tryMulti(ops, responses);
    if (code != Coordination::Error::ZOK
        && code != Coordination::Error::ZNODEEXISTS)
        throw Coordination::Exception(code);
}


String DDLWorker::enqueueQuery(DDLLogEntry & entry)
{
    if (entry.hosts.empty())
        throw Exception("Empty host list in a distributed DDL task", ErrorCodes::LOGICAL_ERROR);

    auto zookeeper = getAndSetZooKeeper();

    String query_path_prefix = queue_dir + "/query-";
    zookeeper->createAncestors(query_path_prefix);

    String node_path = zookeeper->create(query_path_prefix, entry.toString(), zkutil::CreateMode::PersistentSequential);

    /// We cannot create status dirs in a single transaction with previous request,
    /// because we don't know node_path until previous request is executed.
    /// Se we try to create status dirs here or later when we will execute entry.
    try
    {
        createStatusDirs(node_path, zookeeper);
    }
    catch (...)
    {
        LOG_INFO(log, "An error occurred while creating auxiliary ZooKeeper directories in {} . They will be created later. Error : {}", node_path, getCurrentExceptionMessage(true));
    }

    return node_path;
}


void DDLWorker::initializeMainThread()
{
    do
    {
        try
        {
            auto zookeeper = getAndSetZooKeeper();
            zookeeper->createAncestors(queue_dir + "/");
            initialized = true;
        }
        catch (const Coordination::Exception & e)
        {
            if (!Coordination::isHardwareError(e.code))
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected ZooKeeper error: {}", e.message());

            tryLogCurrentException(__PRETTY_FUNCTION__);

            /// Avoid busy loop when ZooKeeper is not available.
            sleepForSeconds(5);
        }
        catch (...)
        {
            tryLogCurrentException(log, "Cannot initialize main thread of DDLWorker, will try again");
            sleepForSeconds(5);
        }
    }
    while (!initialized && !stop_flag);
}

void DDLWorker::runMainThread()
{
    setThreadName("DDLWorker");
    attachToThreadGroup();
    LOG_DEBUG(log, "Starting DDLWorker thread");

    while (!stop_flag)
    {
        try
        {
            /// Reinitialize DDLWorker state (including ZooKeeper connection) if required
            if (!initialized)
            {
                initializeMainThread();
                LOG_DEBUG(log, "Initialized DDLWorker thread");
            }

            cleanup_event->set();
            scheduleTasks();

            LOG_DEBUG(log, "Waiting for queue updates");
            queue_updated_event->wait();
        }
        catch (const Coordination::Exception & e)
        {
            if (Coordination::isHardwareError(e.code))
            {
                initialized = false;
            }
            else if (e.code == Coordination::Error::ZNONODE)
            {
                // TODO add comment: when it happens and why it's expected?
                // maybe because cleanup thread may remove nodes inside queue entry which are currently processed
                LOG_ERROR(log, "ZooKeeper error: {}", getCurrentExceptionMessage(true));
            }
            else
            {
                LOG_ERROR(log, "Unexpected ZooKeeper error: {}.", getCurrentExceptionMessage(true));
                assert(false);
            }
        }
        catch (...)
        {
            tryLogCurrentException(log, "Unexpected error, will try to restart main thread:");
            initialized = false;
        }
    }
}


void DDLWorker::runCleanupThread()
{
    setThreadName("DDLWorkerClnr");
    LOG_DEBUG(log, "Started DDLWorker cleanup thread");

    Int64 last_cleanup_time_seconds = 0;
    while (!stop_flag)
    {
        try
        {
            cleanup_event->wait();
            if (stop_flag)
                break;

            Int64 current_time_seconds = Poco::Timestamp().epochTime();
            if (last_cleanup_time_seconds && current_time_seconds < last_cleanup_time_seconds + cleanup_delay_period)
            {
                LOG_TRACE(log, "Too early to clean queue, will do it later.");
                continue;
            }

            /// ZooKeeper connection is recovered by main thread. We will wait for it on cleanup_event.
            auto zookeeper = tryGetZooKeeper();
            if (zookeeper->expired())
                continue;

            cleanupQueue(current_time_seconds, zookeeper);
            last_cleanup_time_seconds = current_time_seconds;
        }
        catch (...)
        {
            tryLogCurrentException(log, __PRETTY_FUNCTION__);
        }
    }
}

}

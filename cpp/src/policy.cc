#include "policy.h"
#include <algorithm> // For std::remove_if
#include <optional>  // For std::optional
#include <vector>    // For std::vector
#include <limits>    // Required for std::numeric_limits // <-- This is the fix!

// 定义调度器常量
namespace SchedulerConstants {
    const int NUM_PRIORITY_QUEUES = 4; // 反馈队列的数量
    const double IO_END_PRIORITY_BOOST_FACTOR = 0.4; // IO结束任务的优先级加成因子
    const double REGULAR_TASK_PRIORITY_FACTOR = 1.0; // 普通任务的优先级因子
    const double OVERDUE_PENALTY_SCORE = 1e5; // 超时任务的惩罚分数
    const int IDLE_TASK_ID = 0; // 表示CPU或IO空闲的Task ID
}

// 全局变量，表示当前模拟时间
int g_currentTime = 0;

// 四级反馈队列（CPU任务）
std::vector<Event::Task> g_cpuTaskQueues[SchedulerConstants::NUM_PRIORITY_QUEUES];
// 四级反馈队列（IO任务）
std::vector<Event::Task> g_ioTaskQueues[SchedulerConstants::NUM_PRIORITY_QUEUES];

// 辅助函数：从队列中移除特定ID的任务
void removeTaskFromQueues(std::vector<Event::Task> queues[], int taskId) {
    for (int i = 0; i < SchedulerConstants::NUM_PRIORITY_QUEUES; ++i) {
        queues[i].erase(
            std::remove_if(queues[i].begin(), queues[i].end(),
                           [&](const Event::Task& t) { return t.taskId == taskId; }),
            queues[i].end());
    }
}

/**
 * @brief 根据“未超时优先 + 刚结束 IO 的任务优先”的策略选择任务，并将其在队列间迁移。
 * @param taskQueues 任务队列数组（CPU或IO队列）。
 * @param fromLevel 源优先级队列等级。
 * @param toLevel 目标优先级队列等级。
 * @param currentTime 当前时间。
 * @return 选中的任务（如果存在）。
 */
std::optional<Event::Task> selectAndMigrateTask(std::vector<Event::Task> taskQueues[],
                                                int fromLevel, int toLevel, int currentTime) {
    std::optional<Event::Task> selectedTask;
    // 使用最大值初始化最小分数，需要包含 <limits> 头文件
    double minScore = std::numeric_limits<double>::max(); 

    // 遍历当前等级的任务，计算得分并选择最优任务
    for (const auto& task : taskQueues[fromLevel]) {
        double score;

        if (task.deadline > currentTime) {
            // 任务未超时：根据是否刚结束IO给予不同优先级
            if (task.status == Event::Task::Status::kIoEnd) {
                // 刚从IO返回的任务，优先级更高 (得分更低)
                score = SchedulerConstants::IO_END_PRIORITY_BOOST_FACTOR * (task.deadline - currentTime);
            } else {
                // 普通未超时任务
                score = SchedulerConstants::REGULAR_TASK_PRIORITY_FACTOR * (task.deadline - currentTime);
            }
        } else {
            // 任务已超时：给予高额惩罚分数，使其优先级极低
            score = SchedulerConstants::OVERDUE_PENALTY_SCORE + (task.deadline - currentTime);
        }

        // 选择得分最小的任务
        if (score < minScore) {
            minScore = score;
            selectedTask = task;
        }
    }

    // 如果找到了任务，则将其从源队列移除，并添加到目标队列
    if (selectedTask.has_value()) {
        taskQueues[fromLevel].erase(
            std::remove_if(taskQueues[fromLevel].begin(), taskQueues[fromLevel].end(),
                           [&](const Event::Task& t) { return t.taskId == selectedTask->taskId; }),
            taskQueues[fromLevel].end());
        taskQueues[toLevel].push_back(selectedTask.value());
    }

    return selectedTask;
}

/**
 * @brief 实现多级反馈队列调度策略。
 * @param events 当前周期内发生的所有事件。
 * @param currentCpuTask 当前CPU上运行的任务ID (0表示空闲)。
 * @param currentIoTask 当前IO设备上运行的任务ID (0表示空闲)。
 * @return 调度器决定的下一步动作（哪些任务应该运行）。
 */
Action policy(const std::vector<Event>& events, int currentCpuTask, int currentIoTask) {
    Action resultAction;
    resultAction.cpuTask = currentCpuTask; // 默认保持当前CPU任务不变
    resultAction.ioTask = currentIoTask;   // 默认保持当前IO任务不变

    // --- 步骤1: 处理所有新发生的事件，更新任务队列状态 ---
    for (const auto& event : events) {
        g_currentTime = event.time; // 更新当前模拟时间
        Event::Task currentEventTask = event.task; // 获取事件关联的任务副本

        switch (event.type) {
            case Event::Type::kTaskArrival:
                // 任务到达：根据优先级加入CPU队列
                // 高优先级任务放入队列0，低优先级任务放入队列2
                g_cpuTaskQueues[(currentEventTask.priority == Event::Task::Priority::kHigh) ? 0 : 2].push_back(currentEventTask);
                break;

            case Event::Type::kIoRequest:
                // 任务请求IO：从CPU队列中移除，加入IO队列
                removeTaskFromQueues(g_cpuTaskQueues, currentEventTask.taskId);
                g_ioTaskQueues[(currentEventTask.priority == Event::Task::Priority::kHigh) ? 0 : 2].push_back(currentEventTask);
                break;

            case Event::Type::kIoEnd:
                // 任务IO结束：从IO队列中移除，标记为IO结束状态，并重新加入CPU队列
                removeTaskFromQueues(g_ioTaskQueues, currentEventTask.taskId);
                currentEventTask.status = Event::Task::Status::kIoEnd; // 标记为IO结束，以便获得CPU优先级加成
                g_cpuTaskQueues[(currentEventTask.priority == Event::Task::Priority::kHigh) ? 0 : 2].push_back(currentEventTask);
                break;

            case Event::Type::kTaskFinish:
                // 任务完成：从CPU队列中移除
                removeTaskFromQueues(g_cpuTaskQueues, currentEventTask.taskId);
                break;

            case Event::Type::kTimer:
                // 计时器事件：通常用于触发调度，但在此策略中主要依赖其他事件驱动
                // 无需特殊处理，因为任务选择逻辑会在最后执行
                break;

            default:
                // 未知事件类型，忽略
                break;
        }
    }

    // --- 步骤2: 选择下一个CPU上要运行的任务 ---
    // 遍历CPU任务队列，从最高优先级（0级）开始查找
    for (int i = 0; i < SchedulerConstants::NUM_PRIORITY_QUEUES; ++i) {
        // 计算下一个等级：如果是最低级（3），则回到最高级（0），否则进入下一级
        int nextLevel = (i == SchedulerConstants::NUM_PRIORITY_QUEUES - 1) ? 0 : i + 1;
        auto chosenCpuTask = selectAndMigrateTask(g_cpuTaskQueues, i, nextLevel, g_currentTime);
        if (chosenCpuTask.has_value()) {
            resultAction.cpuTask = chosenCpuTask->taskId; // 找到了CPU任务，更新结果
            break; // 找到一个就停止查找
        }
    }

    // --- 步骤3: 选择下一个IO设备上要运行的任务（仅当IO设备空闲时） ---
    if (currentIoTask == SchedulerConstants::IDLE_TASK_ID) {
        // 遍历IO任务队列，从最高优先级（0级）开始查找
        for (int i = 0; i < SchedulerConstants::NUM_PRIORITY_QUEUES; ++i) {
            // 计算下一个等级（同CPU任务）
            int nextLevel = (i == SchedulerConstants::NUM_PRIORITY_QUEUES - 1) ? 0 : i + 1;
            auto chosenIoTask = selectAndMigrateTask(g_ioTaskQueues, i, nextLevel, g_currentTime);
            if (chosenIoTask.has_value()) {
                resultAction.ioTask = chosenIoTask->taskId; // 找到了IO任务，更新结果
                break; // 找到一个就停止查找
            }
        }
    }

    return resultAction;
}

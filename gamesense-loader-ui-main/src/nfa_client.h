#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct nfa_event
{
    std::string type;
    std::vector<std::string> fields;
};

// All service work happens in the source-built C# worker, off the render thread.
// The inherited anonymous pipes are private to this parent/child pair.
class nfa_client
{
public:
    ~nfa_client();
    bool start();
    bool send(const std::string& command, const std::vector<std::string>& args = {});
    std::vector<nfa_event> poll();
    bool busy() const { return busy_; }
    bool connected() const { return connected_; }
private:
    void receive();
    void push(nfa_event event);
    HANDLE process_ = nullptr, input_ = nullptr, output_ = nullptr, job_ = nullptr;
    std::thread reader_;
    std::mutex mutex_;
    std::vector<nfa_event> events_;
    std::atomic<bool> busy_{ false }, connected_{ false }, stopping_{ false };
};

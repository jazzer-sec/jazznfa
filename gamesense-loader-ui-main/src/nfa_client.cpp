#include "nfa_client.h"
#include "runtime_bundle.h"
#include <wincrypt.h>
#include <filesystem>
#include <utility>

namespace
{
    std::string encode(const std::string& value)
    {
        if (value.empty()) return {};
        DWORD size = 0;
        CryptBinaryToStringA(reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size);
        std::string result(size, '\0');
        CryptBinaryToStringA(reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &size);
        result.resize(size);
        return result;
    }

    std::string decode(const std::string& value)
    {
        if (value.empty()) return {};
        DWORD size = 0;
        if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) return {};
        std::string result(size, '\0');
        if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64,
            reinterpret_cast<BYTE*>(result.data()), &size, nullptr, nullptr)) return {};
        result.resize(size);
        return result;
    }
}

nfa_client::~nfa_client()
{
    stopping_ = true;
    if (input_) CloseHandle(input_); // EOF asks an idle worker to exit.
    if (process_)
    {
        if (WaitForSingleObject(process_, 1500) == WAIT_TIMEOUT) TerminateProcess(process_, 0);
        WaitForSingleObject(process_, 1500);
    }
    if (reader_.joinable()) reader_.join();
    if (output_) CloseHandle(output_);
    if (process_) CloseHandle(process_);
    if (job_) CloseHandle(job_);
}

void nfa_client::push(nfa_event event)
{
    std::lock_guard lock(mutex_);
    events_.push_back(std::move(event));
}

bool nfa_client::start()
{
    std::filesystem::path backend;
    try { backend = runtime_bundle::backend(); }
    catch (const std::exception& error)
    {
        push({ "error", { error.what() } });
        return false;
    }
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE child_input = nullptr, child_output = nullptr;
    if (!CreatePipe(&child_input, &input_, &security, 0)) return false;
    if (!CreatePipe(&output_, &child_output, &security, 0))
    {
        CloseHandle(child_input); CloseHandle(input_); input_ = nullptr;
        return false;
    }
    SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);

    // Restrict inheritance to the two pipe ends, even if another component has
    // inheritable handles in this process.
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<BYTE> attributes(bytes);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    InitializeProcThreadAttributeList(list, 1, 0, &bytes);
    HANDLE handles[] = { child_input, child_output };
    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input;
    startup.StartupInfo.hStdOutput = child_output;
    startup.StartupInfo.hStdError = child_output;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + backend.wstring() + L"\" --native-bridge";
    auto cwd = backend.parent_path().wstring();
    bool launched = CreateProcessW(backend.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.c_str(), &startup.StartupInfo, &process) != FALSE;
    DeleteProcThreadAttributeList(list);
    CloseHandle(child_input); CloseHandle(child_output);
    if (!launched)
    {
        push({ "error", { "Could not start NFA services (Windows error " + std::to_string(GetLastError()) + ")." } });
        CloseHandle(input_); CloseHandle(output_); input_ = output_ = nullptr;
        return false;
    }
    process_ = process.hProcess;
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    // Own only the worker. Steam and the cloud-sync helper must survive the UI.
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if (job_)
    {
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        AssignProcessToJobObject(job_, process_);
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    connected_ = true;
    reader_ = std::thread(&nfa_client::receive, this);
    return send("hello");
}

bool nfa_client::send(const std::string& command, const std::vector<std::string>& args)
{
    if (!connected_ || busy_.exchange(true)) return false;
    std::string line = command;
    for (const auto& arg : args) line += '\t' + encode(arg);
    line += '\n';
    DWORD written = 0;
    if (!WriteFile(input_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size())
    {
        busy_ = false;
        connected_ = false;
        push({ "error", { "Connection to NFA services was lost. Restart the tool." } });
        return false;
    }
    return true;
}

void nfa_client::receive()
{
    std::string pending;
    char buffer[8192];
    DWORD count = 0;
    while (ReadFile(output_, buffer, sizeof(buffer), &count, nullptr) && count)
    {
        pending.append(buffer, count);
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos)
        {
            auto line = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto split = line.find('\t');
            nfa_event event{ line.substr(0, split), {} };
            while (split != std::string::npos)
            {
                auto next = line.find('\t', split + 1);
                event.fields.push_back(decode(line.substr(split + 1, next == std::string::npos ? next : next - split - 1)));
                split = next;
            }
            if (event.type == "done") busy_ = false;
            push(std::move(event));
        }
        if (pending.size() > 8 * 1024 * 1024) break;
    }
    connected_ = false;
    busy_ = false;
    if (!stopping_) push({ "error", { "NFA service process stopped. Restart the tool; pending purchases remain recoverable." } });
}

std::vector<nfa_event> nfa_client::poll()
{
    std::lock_guard lock(mutex_);
    std::vector<nfa_event> result;
    result.swap(events_);
    return result;
}

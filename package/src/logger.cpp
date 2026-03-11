#include "logger.hpp"
#include <iostream>


logger::logArgs logger::CreateLogArgs(logger::Severity severity, const std::string message, logger::EventLog* eventLog, int eventId = 1000) {
    return logger::logArgs(severity, message, eventId, eventLog);
}


Napi::FunctionReference logger::EventLog::constructor;

Napi::Object logger::EventLog::Init(Napi::Env env, Napi::Object exports) {
    Napi::HandleScope scope(env);

    Napi::Function func = DefineClass(env, "EventLog", {
        InstanceMethod("log", &EventLog::log)
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("EventLog", func);
    return exports;
}

logger::EventLog::EventLog(const Napi::CallbackInfo& info) : Napi::ObjectWrap<logger::EventLog>(info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Invalid Parameters");
    }

    source = info[0].As<Napi::String>();

    std::wstring wideSource(source.begin(), source.end());
    eventLogHandle_ = RegisterEventSourceW(NULL, wideSource.c_str());
    if (!eventLogHandle_) {
        Napi::TypeError::New(env, "Unable to register event source: " + logger::getLastErrorAsString());
    }
}

Napi::Object logger::EventLog::NewInstance(Napi::Env env, Napi::Value arg) {
    Napi::EscapableHandleScope scope(env);
    Napi::Object obj = constructor.New({ arg });
    return scope.Escape(napi_value(obj)).ToObject();
}

Napi::Value logger::EventLog::log(const Napi::CallbackInfo& info) {
    return logger::log_wrapped(info, this);
}


bool logger::log(logger::Severity severity, const std::string& message, int eventId, logger::EventLog* eventLog, Napi::Env& env) {
    WORD category = 0;
    PSID user = NULL;
    DWORD binDataSize = 0;
    LPVOID binData = NULL;

    HANDLE handle = eventLog->eventLogHandle_;
    DWORD event = eventId;
    WORD type;

    if (!parseSeverity(severity, &type)) {
        Napi::TypeError::New(env, "Failed to parse severity");
    }

    // Convert UTF-8 string to UTF-16
    int wideLength = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    if (wideLength == 0) {
        return false;
    }
    std::wstring wideMessage(wideLength - 1, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wideMessage.data(), wideLength) == 0) {
        return false;
    }

    const WORD numStrings = 1;
    LPCWSTR strings[numStrings];
    strings[0] = wideMessage.c_str();

    auto ret = ReportEventW(handle, type, category, eventId, user, numStrings, binDataSize, strings, binData);
    if (!ret) { std::cout << logger::getLastErrorAsString() << "\n"; }

    return ret;
}



void logger::logWorker::Execute() {
    try {
        result = logger::log(severity, message, eventId, eventLog, Env());

    } catch (std::exception& e) {
        Napi::AsyncWorker::SetError(e.what());
    } catch (...) {
        Napi::AsyncWorker::SetError("An unknown error has occurred");
    }
}

void logger::logWorker::OnOK() {
    deferred.Resolve(Napi::Boolean::New(Env(), result));
}

void logger::logWorker::OnError(Napi::Error const &error) {
    deferred.Reject(error.Value());
}

Napi::Promise logger::logWorker::GetPromise() {
    return deferred.Promise();
}


Napi::Value logger::log_wrapped(const Napi::CallbackInfo& info, logger::EventLog* eventLog) {
    Napi::Env env = info.Env();
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsString() || !info[2].IsNumber()) {
        Napi::TypeError::New(env, "Invalid Parameters");
    }

    Napi::Number severity = info[0].As<Napi::Number>();
    Napi::String message = info[1].As<Napi::String>();
    Napi::Number eventId = info[2].As<Napi::Number>();

    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    try {

        logger::Severity _severity_ = static_cast<logger::Severity>(severity.Int32Value());
        auto args = logger::CreateLogArgs(_severity_, message.Utf8Value(), eventLog, eventId);
        logger::logWorker *worker = new logger::logWorker(env, &args);

        auto promise = worker->GetPromise();
        worker->Queue();
        return promise;

    } catch (...) {
        deferred.Reject(Napi::Error::New(env, "An unexpected error occurred").Value());
        return deferred.Promise();
    }
}
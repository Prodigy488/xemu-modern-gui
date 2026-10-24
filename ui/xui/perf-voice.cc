//
// xemu User Interface - voice commands for Performance Capture
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "perf-voice.hh"
#include "i18n.hh"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace {
std::mutex g_mutex;
std::deque<PerfVoiceCommand> g_queue;
std::string g_status;
bool g_status_ok = false;
std::atomic<bool> g_listening{ false };

void SetStatus(const std::string &s, bool ok)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = s;
    g_status_ok = ok;
}

void Push(PerfVoiceCommand c)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.size() < 8) {
        g_queue.push_back(c);
    }
}
} // namespace

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <sapi.h>

namespace {
// CLSIDs, from sapi51.h (declared there, defined here so no extra
// library is needed)
const CLSID kClsidInprocRecognizer = { 0x41b89b6b, 0x9399, 0x11d2,
    { 0x96, 0x23, 0x00, 0xc0, 0x4f, 0x8e, 0xe6, 0x28 } };
const CLSID kClsidTokenCategory = { 0xa910187f, 0x0c7a, 0x45ac,
    { 0x92, 0xcc, 0x59, 0xed, 0xaf, 0xb7, 0x7b, 0x53 } };
const CLSID kClsidToken = { 0xef411752, 0x3736, 0x4cb4,
    { 0x9c, 0x8c, 0x8e, 0xf4, 0xcc, 0xb5, 0x8e, 0xfe } };
const wchar_t *kCategoryAudioIn =
    L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\AudioInput";

inline ULONGLONG EventFlag(int e)
{
    // SPFEI(): the event bit plus the two reserved check bits
    return (1ULL << e) | (1ULL << SPEI_RESERVED1) | (1ULL << SPEI_RESERVED2);
}

template <typename T> void Release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// A pointer, never destroyed: a std::thread still running when the
// program exits would end it with std::terminate
std::thread *g_thread = nullptr;
HANDLE g_stop = nullptr;

struct Phrase { const wchar_t *text; PerfVoiceCommand cmd; };
const Phrase kPhrases[] = {
    { L"performance capture start", PerfVoiceCommand::Start },
    { L"start performance capture", PerfVoiceCommand::Start },
    { L"performance capture stop", PerfVoiceCommand::Stop },
    { L"stop performance capture", PerfVoiceCommand::Stop },
    { L"performance capture mark", PerfVoiceCommand::Mark },
    { L"performance capture mark glitch", PerfVoiceCommand::Mark },
};

// Is the speech recognizer an English one? (Its "Language" attribute is a
// list of hex language IDs like "409;9".)
bool RecognizerIsEnglish(ISpRecognizer *rec)
{
    ISpObjectToken *token = nullptr;
    if (FAILED(rec->GetRecognizer(&token)) || !token) {
        return true; // can't tell; let it try
    }
    bool english = true;
    ISpDataKey *attrs = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attrs)) && attrs) {
        LPWSTR lang = nullptr;
        if (SUCCEEDED(attrs->GetStringValue(L"Language", &lang)) && lang) {
            english = false;
            for (const wchar_t *p = lang; *p;) {
                unsigned long id = wcstoul(p, nullptr, 16);
                if ((id & 0x3ff) == 0x09) { // LANG_ENGLISH
                    english = true;
                }
                const wchar_t *semi = wcschr(p, L';');
                if (!semi) {
                    break;
                }
                p = semi + 1;
            }
            CoTaskMemFree(lang);
        }
        attrs->Release();
    }
    token->Release();
    return english;
}

void Listen()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool com = SUCCEEDED(hr);
    ISpRecognizer *rec = nullptr;
    ISpObjectTokenCategory *cat = nullptr;
    ISpObjectToken *mic = nullptr;
    ISpRecoContext *ctx = nullptr;
    ISpRecoGrammar *grammar = nullptr;
    LPWSTR mic_id = nullptr;

    do {
        if (FAILED(CoCreateInstance(kClsidInprocRecognizer, nullptr, CLSCTX_ALL,
                                    __uuidof(ISpRecognizer), (void **)&rec))) {
            SetStatus(tr("Windows speech recognition isn't available on this PC."),
                      false);
            break;
        }
        // The default microphone (Windows sound settings)
        if (FAILED(CoCreateInstance(kClsidTokenCategory, nullptr, CLSCTX_ALL,
                                    __uuidof(ISpObjectTokenCategory),
                                    (void **)&cat)) ||
            FAILED(cat->SetId(kCategoryAudioIn, FALSE)) ||
            FAILED(cat->GetDefaultTokenId(&mic_id)) || !mic_id ||
            FAILED(CoCreateInstance(kClsidToken, nullptr, CLSCTX_ALL,
                                    __uuidof(ISpObjectToken), (void **)&mic)) ||
            FAILED(mic->SetId(nullptr, mic_id, FALSE)) ||
            FAILED(rec->SetInput(mic, TRUE))) {
            SetStatus(tr("No microphone found. Check Windows' sound settings."),
                      false);
            break;
        }
        if (FAILED(rec->CreateRecoContext(&ctx)) || !ctx) {
            SetStatus(tr("Windows speech recognition couldn't start. Is "
                         "microphone access allowed in Windows' privacy "
                         "settings?"),
                      false);
            break;
        }
        bool english = RecognizerIsEnglish(rec);
        ULONGLONG interest = EventFlag(SPEI_RECOGNITION);
        if (FAILED(ctx->SetNotifyWin32Event()) ||
            FAILED(ctx->SetInterest(interest, interest)) ||
            FAILED(ctx->CreateGrammar(1, &grammar))) {
            SetStatus(tr("Windows speech recognition couldn't start."), false);
            break;
        }
        // One rule per command, so the rule ID says which was heard
        bool ok = true;
        for (int id = 1; id <= 3 && ok; id++) {
            SPSTATEHANDLE state = nullptr;
            static const wchar_t *names[] = { L"", L"start", L"stop", L"mark" };
            ok = SUCCEEDED(grammar->GetRule(names[id], id,
                                            SPRAF_TopLevel | SPRAF_Active,
                                            TRUE, &state));
            for (const auto &p : kPhrases) {
                if ((int)p.cmd == id && ok) {
                    ok = SUCCEEDED(grammar->AddWordTransition(
                        state, nullptr, p.text, L" ", SPWT_LEXICAL, 1.0f, nullptr));
                }
            }
        }
        if (!ok || FAILED(grammar->Commit(0)) ||
            FAILED(grammar->SetRuleState(nullptr, nullptr, SPRS_ACTIVE))) {
            SetStatus(tr("Windows speech recognition couldn't load the commands."),
                      false);
            break;
        }
        g_listening = true;
        SetStatus(english ? tr("Listening")
                          : tr("Listening. Windows' speech recognizer isn't set "
                               "to English, so the English commands may not be "
                               "understood."),
                  true);
        HANDLE events[2] = { g_stop, ctx->GetNotifyEventHandle() };
        for (;;) {
            DWORD w = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0 + 1) {
                break; // asked to stop, or an error
            }
            SPEVENT ev;
            ULONG fetched = 0;
            while (SUCCEEDED(ctx->GetEvents(1, &ev, &fetched)) && fetched) {
                if (ev.eEventId == SPEI_RECOGNITION &&
                    ev.elParamType == SPET_LPARAM_IS_OBJECT && ev.lParam) {
                    ISpRecoResult *res = (ISpRecoResult *)ev.lParam;
                    SPPHRASE *phrase = nullptr;
                    if (SUCCEEDED(res->GetPhrase(&phrase)) && phrase) {
                        // Game sound or talking can come close: only take
                        // what the recognizer is fairly sure of
                        if (phrase->Rule.Confidence >= SP_NORMAL_CONFIDENCE &&
                            phrase->Rule.ulId >= 1 && phrase->Rule.ulId <= 3) {
                            Push((PerfVoiceCommand)phrase->Rule.ulId);
                        }
                        CoTaskMemFree(phrase);
                    }
                }
                if (ev.elParamType == SPET_LPARAM_IS_OBJECT && ev.lParam) {
                    ((IUnknown *)ev.lParam)->Release();
                } else if ((ev.elParamType == SPET_LPARAM_IS_POINTER ||
                            ev.elParamType == SPET_LPARAM_IS_STRING) &&
                           ev.lParam) {
                    CoTaskMemFree((void *)ev.lParam);
                }
                fetched = 0;
            }
        }
    } while (false);

    g_listening = false;
    if (grammar) {
        grammar->SetRuleState(nullptr, nullptr, SPRS_INACTIVE);
    }
    Release(grammar);
    Release(ctx);
    Release(mic);
    Release(cat);
    Release(rec);
    if (mic_id) {
        CoTaskMemFree(mic_id);
    }
    if (com) {
        CoUninitialize();
    }
}

void StopThread()
{
    if (g_thread && g_thread->joinable()) {
        SetEvent(g_stop);
        g_thread->join();
        delete g_thread;
        g_thread = nullptr;
    }
}
} // namespace

void PerfVoiceSetEnabled(bool enabled)
{
    static bool running = false;
    if (enabled == running) {
        return;
    }
    running = enabled;
    if (!g_stop) {
        g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    StopThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
    }
    if (!enabled) {
        SetStatus("", false);
        return;
    }
    ResetEvent(g_stop);
    SetStatus(tr("Starting..."), true);
    g_thread = new std::thread(Listen);
}

void PerfVoiceShutdown()
{
    StopThread();
}

#else

void PerfVoiceSetEnabled(bool enabled)
{
    SetStatus(enabled ? tr("Voice commands use Windows speech recognition, "
                           "so they're only on Windows.")
                      : "",
              false);
}

void PerfVoiceShutdown() {}

#endif

PerfVoiceCommand PerfVoicePoll()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.empty()) {
        return PerfVoiceCommand::None;
    }
    PerfVoiceCommand c = g_queue.front();
    g_queue.pop_front();
    return c;
}

std::string PerfVoiceStatus(bool *ok)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ok) {
        *ok = g_status_ok;
    }
    return g_status;
}

bool PerfVoiceListening()
{
    return g_listening;
}

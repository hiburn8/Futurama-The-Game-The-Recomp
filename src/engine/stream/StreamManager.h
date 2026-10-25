// The stream manager: the App's queue of file loads (orig: the NiStream part of the App, StreamRequest 0x000D2160 -
// 0x000D2640, the step 0x000D2400 run once per frame). docs/analysis/stream_manager.md.
//
// Requests are appended in creation order; each step garbage-collects requests nobody else holds, asks every new
// request's client for a path, then moves only the first unfinished request by one transition (open -> reading ->
// loaded -> callbacks). A loaded file is offered to the current request and to every later queued / loaded request
// with the same path (_stricmp); a callback that sets the next path keeps its place in the list, so a request's whole
// chain completes before any later request opens a file. A missing file ends the request without a callback.
//
// The port reads a file at the open (the original reads it asynchronously while in state 2); the step cadence is the
// oracle runner's: open in step k, loaded in k + 1, callbacks in k + 2.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"
#include "engine/ni/NiStream.h"

#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fut::stream {

class Request;
class Manager;

// orig: the client interface at request +0x24.
class Client {
public:
    virtual ~Client() = default;
    // vtbl[0]: in state New choose a path (SetPath; false / no path = done); after a load consume
    // `r.Stream()` and set the next path (true) or finish (false).
    virtual bool OnStream(Request& r) = 0;
    virtual void OnProgress(f32 progress) { (void)progress; }   // vtbl[1]
};

enum class State : u32 { New = 0, Queued = 1, Reading = 2, Loaded = 3, Done = 4, Missing = 5 };   // orig: +0x1C

class Request {                                       // orig: 0x28 bytes
public:
    Client* client = nullptr;                         // orig: +0x24
    std::optional<std::string> path;                  // orig: +0x18
    State state = State::New;
    u32 seq = 0;                                      // creation number (event log)
    std::string owner, ownerName;                     // (event log)
    void SetPath(std::string p);                      // orig: 0x000D1F30
    void ClearPath() { path.reset(); }                // orig: 0x000D1F80
    // The loaded file (valid inside the callback after a load only).
    ni::NiStream* Stream() const;
    Manager* manager = nullptr;                       // orig: +0x0C

private:
    friend class Manager;
    bool listRef_ = true;                             // the list's own reference (orig: refcount + 1)
};

class Manager {
public:
    // orig: 0x000D2160: a new request at the tail; the owner keeps the returned reference and releases it in its
    // final callback.
    std::shared_ptr<Request> Create(Client& client, std::string owner = "?", std::string ownerName = "-");
    void Step();                                      // orig: 0x000D2400
    // orig: the first loop of the load check 0x000C53D0: every request done or missing.
    bool AllFinished() const;
    std::function<bool(const std::string& path, std::vector<u8>& out)> readFile;
    std::vector<std::string>* eventLog = nullptr;     // stream_order_oracle.py lines (REQ ... GC)
    // Called with the loaded file after its callbacks instead of dropping it (orig: M.ClearObjects): the port keeps
    // the files whose objects other code still points at without a reference.
    std::function<void(Request& loader, std::unique_ptr<ni::NiStream> file)> keepFile;
    // orig: the global DAT_002E7C24 (the App): the manager new requests go to; nullptr when the caller loads files
    // itself.
    static Manager*& Current();
    // An event of the request owners for the log (STAGE / SPAWN / INIT), stamped with the step.
    void Note(const std::string& l) { Log(l); }
    u32 steps = 0;                                    // manager steps run so far (from 1 inside a step)

private:
    friend class Request;
    void Log(const std::string& l) { if (eventLog) eventLog->push_back(l + " " + std::to_string(steps)); }
    // State Done (logged when `log`) and the list's reference released.
    void Finish(const std::shared_ptr<Request>& r, bool log = true);
    std::list<std::shared_ptr<Request>> list_;        // orig: head M+0xA0, linked +0x10 / +0x14
    std::unique_ptr<ni::NiStream> stream_;            // the manager's NiStream (the loaded file)
    // The file Open read (docs/NOTES.md 2026-10-17): the original's NiUDSStream (0x0013F2E0) reads and builds the
    // objects on its loader fiber, which under the play oracle has run (the objects created and linked, e.g.
    // NiBsp::LinkObject 0x000FCF40) by the next frame's Step entry, not in the Step that opened it.
    std::vector<u8> pendingData_;
    bool pendingLoad_ = false;
    u32 nextSeq_ = 0;
};

}  // namespace fut::stream

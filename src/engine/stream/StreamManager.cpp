// The stream manager; see StreamManager.h.
#include "engine/stream/StreamManager.h"

#include "core/CrtString.h"

#include <algorithm>

namespace fut::stream {

namespace {
std::string Spaced(const std::optional<std::string>& p) {
    if (!p) return "-";
    std::string s = *p;
    std::replace(s.begin(), s.end(), ' ', '_');
    return s;
}
}  // namespace

void Request::SetPath(std::string p) {                // orig: 0x000D1F30 (free + strdup)
    path = std::move(p);
    if (manager) manager->Log("PATH " + std::to_string(seq) + " " + Spaced(path));
}

ni::NiStream* Request::Stream() const { return manager ? manager->stream_.get() : nullptr; }

std::shared_ptr<Request> Manager::Create(Client& client, std::string owner, std::string ownerName) {   // 0x000D2160
    auto r = std::make_shared<Request>();
    r->client = &client;
    r->manager = this;
    r->seq = ++nextSeq_;
    r->owner = std::move(owner);
    r->ownerName = ownerName.empty() ? "-" : std::move(ownerName);
    list_.push_back(r);
    std::string n = r->ownerName;
    std::replace(n.begin(), n.end(), ' ', '_');
    Log("REQ " + std::to_string(r->seq) + " " + r->owner + " " + n);
    return r;
}

Manager*& Manager::Current() {
    static Manager* current = nullptr;
    return current;
}

bool Manager::AllFinished() const {
    for (const auto& r : list_)
        if (r->state != State::Done && r->state != State::Missing) return false;
    return true;
}

void Manager::Finish(const std::shared_ptr<Request>& r, bool log) {
    if (r->state != State::Done) r->state = State::Done;
    if (log) Log("DONE " + std::to_string(r->seq));
    // orig: --refs; at 0 deleted (unlinked). The port drops the list's reference: the request leaves the list once
    // its owner has released it too (owners release inside their final callback).
    r->listRef_ = false;
    if (r.use_count() <= 2) list_.remove(r);          // (this reference + the list's)
}

void Manager::Step() {                                // orig: 0x000D2400
    if (pendingLoad_) {                               // (the file Open read in the previous Step: its objects now)
        pendingLoad_ = false;
        stream_ = std::make_unique<ni::NiStream>();
        (void)stream_->Load(pendingData_);            // (a file that does not parse leaves the stream empty)
        stream_->ReleaseLoadReferences();             // orig: the load's end, 0x000D2380 on the object array
        pendingData_.clear();
    }
    ++steps;
    // Phase A: requests only the list still holds.
    for (auto it = list_.begin(); it != list_.end();) {
        if ((*it)->listRef_ && it->use_count() == 1) {
            Log("GC " + std::to_string((*it)->seq));
            it = list_.erase(it);
            stream_.reset();                          // orig: M.ClearObjects 0x000D2380
            it = list_.begin();
            continue;
        }
        ++it;
    }
    // Phase B: every new request chooses its path.
    for (auto it = list_.begin(); it != list_.end();) {
        std::shared_ptr<Request> r = *it;
        if (r->state != State::New || !r->listRef_) {
            ++it;
            continue;
        }
        const bool ok = r->client && r->client->OnStream(*r);
        Log("RET0 " + std::to_string(r->seq) + " " + (ok ? "1" : "0") + " " + Spaced(r->path));
        if (!ok || !r->path) {
            Finish(r);
            it = list_.begin();                       // orig: restart from the head
            continue;
        }
        r->state = State::Queued;
        ++it;
    }
    // Phase C: the first unfinished request moves by one transition.
    std::shared_ptr<Request> r;
    for (const auto& q : list_)
        if (q->state != State::Done && q->state != State::Missing) {
            r = q;
            break;
        }
    if (!r) return;
    switch (r->state) {
    case State::Queued: {                             // orig: Open 0x000D2280
        Log("OPEN " + std::to_string(r->seq) + " " + Spaced(r->path));
        std::vector<u8> data;
        if (!readFile || !readFile(*r->path, data)) {
            r->state = State::Missing;
            Log("MISSING " + std::to_string(r->seq));
            break;
        }
        pendingData_ = std::move(data);               // (loaded at the next Step's entry, see pendingLoad_)
        pendingLoad_ = true;
        r->state = State::Reading;
        break;
    }
    case State::Loaded: {
        const std::string p = *r->path;
        for (auto it = list_.begin(); it != list_.end();) {
            std::shared_ptr<Request> q = *it;
            if (!q->path || crt::StrICmp(q->path->c_str(), p.c_str()) != 0 ||
                !(q == r || q->state == State::Queued || q->state == State::Loaded)) {
                ++it;
                continue;
            }
            Log("LOAD " + std::to_string(q->seq) + " " + Spaced(q->path) + " " + (q == r ? "current" : "shared"));
            q->ClearPath();
            const bool ok = q->client && q->client->OnStream(*q);
            Log("RET " + std::to_string(q->seq) + " " + (ok ? "1" : "0") + " " + Spaced(q->path));
            if (ok && q->path) {
                q->state = State::Queued;
                ++it;
            } else if (q != r) {
                Finish(q);
                it = list_.begin();
            } else {
                q->state = State::Done;               // (the current request is released below)
                Log("DONE " + std::to_string(q->seq));
                ++it;
            }
        }
        if (keepFile && stream_) keepFile(*r, std::move(stream_));
        stream_.reset();                              // orig: M.ClearObjects: the loaded objects are dropped here
        if (r->state == State::Done) {
            Finish(r, false);
            return;                                   // (no progress call)
        }
        break;
    }
    default:                                          // Reading: the runner's Pump finishes at once
        r->state = State::Loaded;
        break;
    }
    if (r->client) r->client->OnProgress(r->state == State::Done ? 1.0f : 0.0f);   // orig: 0x000D2060 (L: values)
}

}  // namespace fut::stream

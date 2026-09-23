#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#include <atomic>
#include <condition_variable>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Gate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    void block()
    {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void wait()
    {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }), "worker did not progress");
    }
    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
    ~Gate() { release(); }
};

struct Record
{
    std::vector<uint32_t> events;
    std::vector<int> rounding;
    std::thread::id submitThread, presentThread;
    Gate *drawGate = nullptr, *presentGate = nullptr;
    bool fail = false;
};

class Probe : public GSRasterBackend
{
public:
    explicit Probe(Record &record) : r(record) {}
    void Initialize(uint8_t *, uint32_t size) override { add(1, size); }
    void Reset() override { add(2); }
    void Submit(const GSPrimitiveBatch &batch) override
    {
        r.submitThread = std::this_thread::get_id();
        r.rounding.push_back(std::fegetround());
        if (r.drawGate) r.drawGate->block();
        if (r.fail) throw std::runtime_error("worker failure");
        add(3, batch.vertices[0].r);
    }
    void BeginTransfer(const GSTransferCommand &c) override { add(4, c.trxreg.rrw); }
    void UploadImage(const uint8_t *bytes, uint32_t size) override
    {
        add(5, size);
        for (uint32_t i = 0; i < size; ++i) r.events.push_back(bytes[i]);
    }
    void Flush() override { add(6); }
    void TextureFlush() override { add(7); }
    void Sync(GSSyncReason reason) override { add(8, static_cast<uint32_t>(reason)); }
    PresentationFrame Present(const GSPresentationRequest &request) override
    {
        r.presentThread = std::this_thread::get_id();
        if (r.presentGate) r.presentGate->block();
        add(9, static_cast<uint32_t>(request.vsyncTick));
        PresentationFrame frame;
        frame.width = 123;
        frame.pixels = {1, 2, 3, 4};
        return frame;
    }
    bool ClearFramebuffer(const GSContext &, uint32_t color) override { add(10, color); return color == 19; }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t size) override
    {
        add(11, size);
        if (size) *dst = 42;
        return size ? 1u : 0u;
    }
    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t x, uint32_t y) const override { add(12, x + y); return 73; }
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t value) override { add(13, value); }
    void SnapshotVram(std::vector<uint8_t> &out) const override { add(14); out = {17, 31}; }
    GSTransferSnapshot GetTransferSnapshot() const override
    {
        add(15);
        GSTransferSnapshot s;
        s.copiedPixels = 91;
        return s;
    }
private:
    void add(uint32_t op, uint32_t value = 0) const { r.events.push_back(op); r.events.push_back(value); }
    Record &r;
};

void sequence(GSRasterBackend &backend)
{
    backend.Initialize(nullptr, 0);
    GSPrimitiveBatch draw;
    draw.vertices[0].r = 17;
    backend.Submit(draw);
    GSTransferCommand transfer;
    transfer.trxreg.rrw = 123;
    backend.BeginTransfer(transfer);
    uint8_t bytes[] = {42, 17, 29};
    backend.UploadImage(bytes, sizeof(bytes));
    backend.UploadImage(nullptr, 0);
    std::memset(bytes, 0, sizeof(bytes));
    backend.Flush();
    backend.TextureFlush();
    backend.WriteVram(0, 0, 1, 0, 0, 91);
    backend.Sync(GSSyncReason::Finish);
    require(backend.ReadVram(0, 0, 1, 2, 3) == 73, "read result changed");
    std::vector<uint8_t> snapshot;
    backend.SnapshotVram(snapshot);
    require(snapshot == std::vector<uint8_t>({17, 31}), "snapshot changed");
    require(backend.GetTransferSnapshot().copiedPixels == 91, "transfer snapshot changed");
    require(backend.ConsumeLocalToHostBytes(bytes, sizeof(bytes)) == 1 && bytes[0] == 42, "FIFO read changed");
    require(backend.ClearFramebuffer({}, 19), "clear result changed");
    GSPresentationRequest request;
    request.vsyncTick = 789;
    const auto frame = backend.Present(request);
    require(frame.width == 123 && frame.pixels == std::vector<uint8_t>({1, 2, 3, 4}), "presentation changed");
    backend.Reset();
}

void orderedOwnership()
{
    Record direct, threaded;
    Probe reference(direct);
    const int previous = std::fegetround();
    for (int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD}) {
        std::fesetround(mode);
        sequence(reference);
    }
    {
        GSThreadedBackend worker(std::make_unique<Probe>(threaded));
        for (int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD}) {
            std::fesetround(mode);
            sequence(worker);
        }
    }
    std::fesetround(previous);
    require(direct.events == threaded.events, "command order or copied payload changed");
    require(direct.rounding == threaded.rounding, "producer rounding mode changed");
    require(threaded.submitThread != std::this_thread::get_id(), "draw stayed on producer");
    require(threaded.presentThread == std::this_thread::get_id(), "presentation left its caller");
}

void boundedQueue()
{
    Gate gate;
    Record record;
    record.drawGate = &gate;
    GSThreadedBackend worker(std::make_unique<Probe>(record), 1);
    worker.Submit({});
    gate.wait();
    std::promise<void> started;
    auto blocked = std::async(std::launch::async, [&] {
        started.set_value();
        const std::vector<uint8_t> large(8192, 35);
        worker.UploadImage(large.data(), static_cast<uint32_t>(large.size()));
    });
    started.get_future().wait();
    const bool waited = blocked.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    gate.release();
    blocked.get();
    worker.Sync(GSSyncReason::LocalToHost);
    require(waited, "producer outran queue capacity");
    require(record.events[2] == 5 && record.events[3] == 8192 && record.events[4] == 35, "oversized packet changed");
}

void presentationBarrier()
{
    Gate gate;
    Record record;
    record.presentGate = &gate;
    GSThreadedBackend worker(std::make_unique<Probe>(record));
    auto present = std::async(std::launch::async, [&] { worker.Present({}); });
    gate.wait();
    std::promise<void> started;
    auto next = std::async(std::launch::async, [&] { started.set_value(); worker.Submit({}); });
    started.get_future().wait();
    const bool waited = next.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    gate.release();
    present.get();
    next.get();
    worker.Sync(GSSyncReason::Finish);
    require(waited, "new draw passed presentation barrier");
    require(record.events == std::vector<uint32_t>({9, 0, 3, 0, 8, static_cast<uint32_t>(GSSyncReason::Finish)}), "presentation reordered draws");
}

void shutdownAndFailure()
{
    Record record;
    {
        GSThreadedBackend worker(std::make_unique<Probe>(record));
        for (unsigned i = 0; i < 5000; ++i) worker.Submit({});
    }
    require(record.events.size() == 10000, "shutdown lost queued draws");
    Record failure;
    failure.fail = true;
    GSThreadedBackend worker(std::make_unique<Probe>(failure));
    worker.Submit({});
    bool caught = false;
    try { worker.Sync(GSSyncReason::Finish); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "worker failure") == 0; }
    require(caught, "worker failure was lost");
}

struct PreparedProbe : Probe
{
    struct Frame final : GSPreparedPresentation
    {
        uint32_t draw = 0;
        uint64_t tick = 0;
        int rounding = 0;
    };
    explicit PreparedProbe(Record &record) : Probe(record) {}
    std::atomic<uint32_t> latestDraw{0};
    Gate *prepareGate = nullptr, *displayGate = nullptr;
    bool failPrepare = false, failDisplay = false;
    std::atomic<bool> cancelled{false};
    void Submit(const GSPrimitiveBatch &batch) override
    {
        Probe::Submit(batch);
        latestDraw = batch.vertices[0].r;
    }
    bool SupportsPreparedPresentation() const override { return true; }
    GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override
    {
        if (prepareGate) prepareGate->block();
        if (failPrepare || cancelled) throw std::runtime_error("prepare failure");
        auto frame = std::make_shared<Frame>();
        frame->draw = latestDraw;
        frame->tick = request.vsyncTick;
        frame->rounding = std::fegetround();
        return frame;
    }
    PresentationFrame DisplayPreparedPresentation(const GSPresentationTicket &ticket) override
    {
        if (displayGate) displayGate->block();
        if (failDisplay) throw std::runtime_error("display failure");
        const auto &prepared = dynamic_cast<const Frame &>(*ticket);
        PresentationFrame frame;
        frame.width = prepared.draw;
        frame.height = static_cast<uint32_t>(prepared.tick);
        frame.rowPitchBytes = static_cast<uint32_t>(prepared.rounding);
        frame.mode = GSPresentationMode::HostPixels;
        frame.pixels = {1, 2, 3, 4};
        return frame;
    }
    void CancelPreparedPresentations() noexcept override
    {
        cancelled = true;
        if (prepareGate) prepareGate->release();
    }
};

void orderedPresentationTicket()
{
    Gate prepareGate, displayGate;
    Record record;
    auto probe = std::make_unique<PreparedProbe>(record);
    auto *raw = probe.get();
    raw->prepareGate = &prepareGate;
    raw->displayGate = &displayGate;
    GSThreadedBackend worker(std::move(probe));
    GSPrimitiveBatch draw;
    draw.vertices[0].r = 17;
    worker.Submit(draw);
    GSPresentationRequest request;
    request.vsyncTick = 123;
    const int rounding = std::fegetround();
    std::fesetround(FE_DOWNWARD);
    auto ticket = worker.PreparePresentation(request);
    std::fesetround(rounding);
    request.vsyncTick = 999;
    prepareGate.wait();
    auto display = std::async(std::launch::async, [&] { return worker.DisplayPreparedPresentation(ticket); });
    auto later = std::async(std::launch::async, [&] {
        draw.vertices[0].r = 29;
        worker.Submit(draw);
    });
    const bool enqueued = later.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    prepareGate.release();
    displayGate.wait();
    auto drained = std::async(std::launch::async, [&] { worker.Sync(GSSyncReason::Finish); });
    const bool progressed = drained.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    displayGate.release();
    later.get();
    drained.get();
    const auto frame = display.get();
    require(enqueued, "presentation wait retained producer submission lock");
    require(progressed && raw->latestDraw == 29, "window display blocked following GS work");
    require(frame.width == 17 && frame.height == 123, "ticket observed a later draw or mutable request");
    require(ticket->sourceVsyncTick == 123, "ticket lost its guest VBlank metadata");
    require(frame.rowPitchBytes == FE_DOWNWARD, "preparation lost caller rounding mode");
    require(worker.DisplayPreparedPresentation(ticket).width == 17, "repeated ticket changed frame");
    require(record.rounding.back() == rounding, "preparation rounding leaked into later draw");
    const std::vector<uint32_t> expected = {3, 17, 6, 0, 8,
        static_cast<uint32_t>(GSSyncReason::Presentation), 3, 29, 8,
        static_cast<uint32_t>(GSSyncReason::Finish)};
    require(record.events == expected, "ticket changed GS observation ordering");
}

void presentationTicketFailures()
{
    Gate gate;
    Record record;
    auto probe = std::make_unique<PreparedProbe>(record);
    probe->prepareGate = &gate;
    probe->failPrepare = true;
    GSThreadedBackend worker(std::move(probe));
    auto first = worker.PreparePresentation({});
    gate.wait();
    auto second = worker.PreparePresentation({});
    gate.release();
    for (const auto &ticket : {first, second}) {
        bool failed = false;
        try { worker.DisplayPreparedPresentation(ticket); }
        catch (const std::runtime_error &e) { failed = std::strcmp(e.what(), "prepare failure") == 0; }
        require(failed, "queued preparation did not propagate worker failure");
    }
    Record displayRecord;
    auto displayProbe = std::make_unique<PreparedProbe>(displayRecord);
    displayProbe->failDisplay = true;
    GSThreadedBackend displayWorker(std::move(displayProbe));
    bool failed = false;
    try { displayWorker.Present({}); }
    catch (const std::runtime_error &e) { failed = std::strcmp(e.what(), "display failure") == 0; }
    require(failed, "window display error was lost");
    bool wrongOwner = false;
    try { displayWorker.DisplayPreparedPresentation(first); }
    catch (const std::invalid_argument &) { wrongOwner = true; }
    require(wrongOwner, "ticket from another backend was accepted");
    Gate blockedGate;
    Record blockedRecord;
    auto blockedProbe = std::make_unique<PreparedProbe>(blockedRecord);
    blockedProbe->prepareGate = &blockedGate;
    auto blockedWorker = std::make_unique<GSThreadedBackend>(std::move(blockedProbe));
    auto held = blockedWorker->PreparePresentation({});
    blockedGate.wait();
    blockedWorker.reset();
}

void frontendPresentationLifecycle(bool replace)
{
    Gate displayGate;
    Record record;
    GSRegisters registers{};
    registers.vsyncTick = 1;
    std::vector<uint8_t> memory(64);
    GS gs;
    gs.init(memory.data(), static_cast<uint32_t>(memory.size()), &registers);
    auto probe = std::make_unique<PreparedProbe>(record);
    probe->latestDraw = 1;
    probe->displayGate = &displayGate;
    gs.setRasterBackend(std::make_unique<GSThreadedBackend>(std::move(probe)));
    auto display = std::async(std::launch::async, [&] { gs.latchHostPresentationFrame(); });
    displayGate.wait();
    auto producer = std::async(std::launch::async, [&] { gs.writeRegister(0, 0); });
    const bool progressed = producer.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    auto lifecycle = std::async(std::launch::async, [&] {
        if (replace) gs.setRasterBackend(nullptr);
        else gs.reset();
    });
    const bool guarded = lifecycle.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout;
    displayGate.release();
    producer.get();
    display.get();
    lifecycle.get();
    require(progressed, "display held frontend state against producer");
    require(guarded, "reset or backend replacement passed in-flight display");
    std::vector<uint8_t> pixels;
    uint32_t width, height;
    if (!replace)
        require(!gs.copyLatchedHostPresentationFrame(pixels, width, height), "reset was followed by stale frame publication");
}

void frontendEnqueueSnapshot()
{
    struct EnqueueProbe final : PreparedProbe
    {
        Gate &enqueueGate;
        EnqueueProbe(Record &record, Gate &gate) : PreparedProbe(record), enqueueGate(gate) {}
        bool QueuesPreparedPresentation() const override { return true; }
        GSPresentationTicket PreparePresentation(const GSPresentationRequest &request) override
        {
            enqueueGate.block();
            return PreparedProbe::PreparePresentation(request);
        }
    };
    Gate enqueueGate;
    Record record;
    GSRegisters registers{};
    registers.vsyncTick = 1;
    std::vector<uint8_t> memory(64);
    GS gs;
    gs.init(memory.data(), static_cast<uint32_t>(memory.size()), &registers);
    gs.setRasterBackend(std::make_unique<EnqueueProbe>(record, enqueueGate));
    auto display = std::async(std::launch::async, [&] { gs.latchHostPresentationFrame(); });
    enqueueGate.wait();
    auto producer = std::async(std::launch::async, [&] { gs.writeRegister(0, 0); });
    const bool atomic = producer.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout;
    enqueueGate.release();
    display.get();
    producer.get();
    require(atomic, "frontend request capture and preparation enqueue were not atomic");
}
} // namespace

int main()
{
    try
    {
        orderedOwnership();
        boundedQueue();
        presentationBarrier();
        shutdownAndFailure();
        orderedPresentationTicket();
        presentationTicketFailures();
        frontendPresentationLifecycle(false);
        frontendPresentationLifecycle(true);
        frontendEnqueueSnapshot();
        std::puts("PASS GS worker: ordering, ownership, barriers, bounded queue, shutdown, failure, presentation tickets");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "FAIL GS worker: %s\n", error.what());
        return 1;
    }
}

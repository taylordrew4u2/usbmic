#include "TestFramework.h"
#include "Core/FilesystemStatusProbe.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace mma;

namespace {

struct TemporaryFolder
{
    TemporaryFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
                 / ("sobstage-status-probe-" + std::to_string (stamp));
        std::filesystem::create_directories (path);
    }

    ~TemporaryFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all (path, ignored);
    }

    std::filesystem::path path;
};

void writeBytes (const std::filesystem::path& path, int count)
{
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < count; ++i)
        out.put ('x');
}

} // namespace

TEST_CASE (FilesystemStatusProbe_SamplesSpaceAndFilesOnItsWorker)
{
    const auto callerThread = std::this_thread::get_id();
    TemporaryFolder folder;
    writeBytes (folder.path / "01_Alice.wav", 123);
    writeBytes (folder.path / "MIX.wav", 456);

    FilesystemStatusProbe probe (std::chrono::milliseconds (20));
    FilesystemStatusProbe::Request request;
    request.destinationPath = folder.path.string();
    request.sessionFolder = folder.path.string();
    request.mirrorPath = folder.path.string();
    request.bytesPerSecond = 1000.0;
    probe.setRequest (request);

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));
    REQUIRE (snapshot.ready);
    REQUIRE (snapshot.filesObservationReady);
    REQUIRE (snapshot.request == request);
    REQUIRE (snapshot.sampledOnThread != callerThread);
    REQUIRE (snapshot.remainingSeconds >= 0.0);
    REQUIRE (snapshot.mirrorFreeBytes >= 0);
    REQUIRE (snapshot.files.size() == 2);
    REQUIRE (snapshot.files[0].name == std::string ("MIX.wav"));
    REQUIRE (snapshot.files[0].sizeBytes == 456);
    REQUIRE (snapshot.files[1].name == std::string ("01_Alice.wav"));
    REQUIRE (snapshot.files[1].sizeBytes == 123);
}

TEST_CASE (FilesystemStatusProbe_DoesNotCallACompletedFilesystemErrorAnEmptyFolder)
{
    TemporaryFolder folder;

    // A single component far beyond supported filesystem limits makes the
    // native status query fail rather than proving that the folder is empty.
    const auto invalidPath = (folder.path / std::string (32768, 'x')).string();

    FilesystemStatusProbe probe (std::chrono::milliseconds (20));
    probe.setRequest ({ {}, invalidPath, {}, 0.0 });

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));
    REQUIRE (snapshot.ready);
    REQUIRE_FALSE (snapshot.filesObservationReady);
    REQUIRE (snapshot.files.empty());
}

TEST_CASE (FilesystemStatusProbe_RefreshesAnActiveFolderWithoutMessageThreadIO)
{
    TemporaryFolder folder;
    const auto mix = folder.path / "MIX.wav";
    writeBytes (mix, 16);

    FilesystemStatusProbe probe (std::chrono::milliseconds (10));
    probe.setRequest ({ folder.path.string(), folder.path.string(), {}, 1000.0 });

    FilesystemStatusProbe::Snapshot before;
    REQUIRE (probe.waitForRevisionAfter (0, before, std::chrono::seconds (2)));
    REQUIRE (before.files.size() == 1);

    writeBytes (mix, 128);
    auto revision = before.revision;
    FilesystemStatusProbe::Snapshot after;
    bool sawGrowth = false;

    for (int attempt = 0; attempt < 20 && ! sawGrowth; ++attempt)
    {
        if (! probe.waitForRevisionAfter (revision, after, std::chrono::milliseconds (100)))
            continue;
        revision = after.revision;
        sawGrowth = ! after.files.empty() && after.files[0].sizeBytes == 128;
    }

    REQUIRE (sawGrowth);
}

TEST_CASE (FilesystemStatusProbe_EventuallyPublishesTheLatestReplacedRequest)
{
    TemporaryFolder folder;
    FilesystemStatusProbe probe (std::chrono::milliseconds (10));

    probe.setRequest ({ "/a/path/that/does/not/exist", {}, {}, 1000.0 });
    probe.setRequest ({ folder.path.string(), {}, {}, 1000.0 });

    FilesystemStatusProbe::Snapshot snapshot;
    uint64_t revision = 0;
    bool sawLatest = false;

    // The first, fast request is allowed to finish before it is replaced. What
    // matters is that the worker promptly catches up and does not remain stuck
    // publishing the obsolete destination.
    for (int attempt = 0; attempt < 20 && ! sawLatest; ++attempt)
    {
        if (! probe.waitForRevisionAfter (revision, snapshot, std::chrono::milliseconds (100)))
            continue;

        revision = snapshot.revision;
        sawLatest = snapshot.request.destinationPath == folder.path.string()
                 && snapshot.remainingSeconds >= 0.0;
    }

    REQUIRE (sawLatest);
}

TEST_CASE (FilesystemStatusProbe_StopWakesTheRefreshWait)
{
    FilesystemStatusProbe probe (std::chrono::seconds (5));
    probe.setRequest ({ {}, {}, {}, 0.0 });

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));

    const auto started = std::chrono::steady_clock::now();
    probe.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Comfortably below the five-second refresh sleep, with enough margin for
    // a heavily loaded sanitizer runner.
    REQUIRE (elapsed < std::chrono::seconds (2));
}

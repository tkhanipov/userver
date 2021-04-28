#include <dump/dumper.hpp>

#include <boost/filesystem/operations.hpp>

#include <dump/dump_locator.hpp>
#include <dump/statistics.hpp>
#include <engine/mutex.hpp>
#include <engine/task/task_processor.hpp>
#include <engine/task/task_with_result.hpp>
#include <formats/json/value_builder.hpp>
#include <logging/log.hpp>
#include <testsuite/dump_control.hpp>
#include <utils/async.hpp>
#include <utils/atomic.hpp>

namespace dump {

void ThrowDumpUnimplemented(const std::string& name) {
  YTX_INVARIANT(false, fmt::format("Dumps are unimplemented for {}. "
                                   "See dump::Read, dump::Write",
                                   name));
}

struct Dumper::Impl {
  Impl(const Config& config, std::unique_ptr<OperationsFactory> rw_factory,
       engine::TaskProcessor& fs_task_processor,
       testsuite::DumpControl& dump_control, DumpableEntity& dumpable,
       Dumper& self)
      : static_config(config),
        config(static_config),
        rw_factory(std::move(rw_factory)),
        fs_task_processor(fs_task_processor),
        dump_control(dump_control),
        testsuite_registration(dump_control, self),
        dumpable(dumpable),
        locator(Config{static_config}),
        last_modifying_update(TimePoint{}),
        last_dumped_update(TimePoint{}) {
    UASSERT(this->rw_factory);
  }

  const Config static_config;
  rcu::Variable<Config> config;
  const std::unique_ptr<OperationsFactory> rw_factory;
  engine::TaskProcessor& fs_task_processor;
  testsuite::DumpControl& dump_control;
  [[maybe_unused]] testsuite::DumperRegistrationHolder testsuite_registration;
  DumpableEntity& dumpable;
  DumpLocator locator;
  std::atomic<TimePoint> last_update;
  std::atomic<TimePoint> last_modifying_update;
  std::atomic<TimePoint> last_dumped_update;
  Statistics statistics;
  engine::TaskWithResult<void> dump_task;
  engine::Mutex mutex;
};

Dumper::Dumper(const Config& config,
               std::unique_ptr<OperationsFactory> rw_factory,
               engine::TaskProcessor& fs_task_processor,
               testsuite::DumpControl& dump_control, DumpableEntity& dumpable)
    : impl_(config, std::move(rw_factory), fs_task_processor, dump_control,
            dumpable, *this) {}

Dumper::~Dumper() = default;

const std::string& Dumper::Name() const {
  // It's OK to use `static_config_` here, because `name` is not dynamically
  // configurable.
  return impl_->static_config.name;
}

void Dumper::WriteDumpAsync() {
  std::lock_guard lock(impl_->mutex);
  const auto config = impl_->config.Read();

  DumpAsyncIfNeeded(DumpType::kHonorDumpInterval, *config);
}

std::optional<TimePoint> Dumper::ReadDump() {
  std::lock_guard lock(impl_->mutex);
  const auto config = impl_->config.Read();

  return LoadFromDump(*config);
}

void Dumper::WriteDumpSyncDebug() {
  std::lock_guard lock(impl_->mutex);
  const auto config = impl_->config.Read();

  if (impl_->dump_task.IsValid()) impl_->dump_task.Wait();
  DumpAsyncIfNeeded(DumpType::kForced, *config);
  impl_->dump_task.Get();  // report any exceptions to testsuite
}

void Dumper::ReadDumpDebug() {
  std::lock_guard lock(impl_->mutex);
  const auto config = impl_->config.Read();

  const auto update_time = LoadFromDump(*config);

  if (!update_time) {
    throw Error(fmt::format(
        "{}: failed to read a dump when explicitly requested", Name()));
  }
}

void Dumper::OnUpdateCompleted(TimePoint update_time,
                               bool has_changes_since_last_dump) {
  impl_->last_update = update_time;
  if (has_changes_since_last_dump) {
    impl_->last_modifying_update = update_time;
    impl_->statistics.is_current_from_dump = false;
  }
}

void Dumper::SetConfigPatch(const std::optional<ConfigPatch>& patch) {
  impl_->config.Assign(patch ? impl_->static_config.MergeWith(*patch)
                             : impl_->static_config);
  impl_->locator.SetConfig(impl_->config.ReadCopy());
}

rcu::ReadablePtr<Config> Dumper::GetConfig() const {
  return impl_->config.Read();
}

formats::json::Value Dumper::ExtendStatistics() const {
  return formats::json::ValueBuilder{impl_->statistics}.ExtractValue();
}

void Dumper::CancelWriteTaskAndWait() {
  if (impl_->dump_task.IsValid() && !impl_->dump_task.IsFinished()) {
    LOG_WARNING() << Name() << ": stopping a dump task";
    try {
      impl_->dump_task.RequestCancel();
      impl_->dump_task.Wait();
    } catch (const std::exception& ex) {
      LOG_ERROR() << Name() << ": exception in dump task. Reason: " << ex;
    }
  }
}

bool Dumper::ShouldDump(DumpType type, const Config& config) {
  if (!config.dumps_enabled) {
    LOG_DEBUG() << Name()
                << ": dump skipped, because dumps are disabled for this dumper";
    return false;
  }

  const auto last_update = impl_->last_update.load();

  if (last_update == TimePoint{}) {
    LOG_WARNING() << Name()
                  << ": dump skipped, because no successful updates "
                     "have been performed";
    return false;
  }

  if (type == DumpType::kHonorDumpInterval &&
      impl_->last_dumped_update.load() >
          last_update - config.min_dump_interval) {
    LOG_INFO() << Name()
               << ": dump skipped, because dump interval has not passed yet";
    return false;
  }

  if (impl_->dump_task.IsValid() && !impl_->dump_task.IsFinished()) {
    LOG_INFO() << Name()
               << ": dump skipped, because a previous dump "
                  "write is in progress";
    return false;
  }

  return true;
}

void Dumper::DoDump(TimePoint update_time, ScopeTime& scope) {
  const auto dump_start = std::chrono::steady_clock::now();

  std::uint64_t dump_size;
  try {
    auto dump_stats = impl_->locator.RegisterNewDump(update_time);
    const auto& dump_path = dump_stats.full_path;
    auto writer = impl_->rw_factory->CreateWriter(dump_path, scope);
    impl_->dumpable.GetAndWrite(*writer);
    writer->Finish();
    dump_size = boost::filesystem::file_size(dump_path);
  } catch (const std::exception& ex) {
    LOG_ERROR() << Name() << ": error while writing a dump. Reason: " << ex;
    throw;
  }

  impl_->statistics.last_written_size = dump_size;
  impl_->statistics.last_nontrivial_write_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - dump_start);
  impl_->statistics.last_nontrivial_write_start_time = dump_start;
}

void Dumper::DumpAsync(DumpOperation operation_type) {
  UASSERT_MSG(!impl_->dump_task.IsValid() || impl_->dump_task.IsFinished(),
              "Another dump write task is already running");

  if (impl_->dump_task.IsValid()) {
    try {
      impl_->dump_task.Get();
    } catch (const std::exception& ex) {
      LOG_ERROR() << Name()
                  << ": error from writing a previous dump. Reason: " << ex;
    }
  }

  impl_->dump_task = utils::Async(
      impl_->fs_task_processor, "write-dump",
      [this, operation_type, old_update_time = impl_->last_dumped_update.load(),
       new_update_time = impl_->last_update.load()] {
        try {
          auto scope_time = tracing::Span::CurrentSpan().CreateScopeTime(
              "write-dump/" + Name());

          switch (operation_type) {
            case DumpOperation::kNewDump:
              impl_->locator.Cleanup();
              DoDump(new_update_time, scope_time);
              break;
            case DumpOperation::kBumpTime:
              if (!impl_->locator.BumpDumpTime(old_update_time,
                                               new_update_time)) {
                DoDump(new_update_time, scope_time);
              }
              break;
          }

          impl_->last_dumped_update = new_update_time;
        } catch (const std::exception& ex) {
          LOG_ERROR() << Name() << ": failed to write a dump. Reason: " << ex;
          throw;
        }
      });
}

void Dumper::DumpAsyncIfNeeded(DumpType type, const Config& config) {
  if (!ShouldDump(type, config)) {
    if (type == DumpType::kForced) {
      throw Error(fmt::format(
          "{}: not ready to write a dump, see logs for details", Name()));
    }
    return;
  }

  if (impl_->last_dumped_update.load() == impl_->last_modifying_update.load()) {
    // If nothing has been updated since the last time, skip the serialization
    // and dump processes by just renaming the dump file.
    LOG_DEBUG() << Name() << ": skipped dump, because nothing has been updated";
    DumpAsync(DumpOperation::kBumpTime);
  } else {
    DumpAsync(DumpOperation::kNewDump);
  }
}

std::optional<TimePoint> Dumper::LoadFromDump(const Config& config) {
  tracing::Span span("load-from-dump/" + Name());
  const auto load_start = std::chrono::steady_clock::now();

  if (!config.dumps_enabled) {
    LOG_DEBUG() << Name()
                << ": could not load a dump, because dumps are disabled for "
                   "this dumper";
    return {};
  }

  const std::optional<TimePoint> update_time =
      utils::Async(impl_->fs_task_processor, "read-dump", [this] {
        try {
          auto dump_stats = impl_->locator.GetLatestDump();
          if (!dump_stats) return std::optional<TimePoint>{};

          auto reader = impl_->rw_factory->CreateReader(dump_stats->full_path);
          impl_->dumpable.ReadAndSet(*reader);
          reader->Finish();

          return std::optional{dump_stats->update_time};
        } catch (const std::exception& ex) {
          LOG_ERROR() << Name()
                      << ": error while reading a dump. Reason: " << ex;
          return std::optional<TimePoint>{};
        }
      }).Get();

  if (!update_time) return {};

  LOG_INFO() << Name() << ": a dump has been loaded successfully";
  impl_->last_update = *update_time;
  impl_->last_modifying_update = *update_time;
  utils::AtomicMax(impl_->last_dumped_update, *update_time);

  impl_->statistics.is_loaded = true;
  impl_->statistics.is_current_from_dump = true;
  impl_->statistics.load_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - load_start);
  return update_time;
}

}  // namespace dump

#include "module.h"

namespace mig {
namespace {

class SimpleModule : public IModule {
 public:
  SimpleModule(std::string name, bool compiled)
      : name_(std::move(name)), compiled_(compiled) {}
  std::string name() const override { return name_; }
  bool compiled() const override { return compiled_; }
  bool enabled(const Options& o) const override {
    return compiled_ && o.module_enabled(name_);
  }

 private:
  std::string name_;
  bool compiled_;
};

}  // namespace

ModuleManager::ModuleManager() {
  modules_.push_back(std::make_unique<SimpleModule>("pg_cdc",
#ifdef MIG_ENABLE_PG_CDC
                                                    true
#else
                                                    false
#endif
                                                    ));
  modules_.push_back(std::make_unique<SimpleModule>("repair",
#ifdef MIG_ENABLE_REPAIR
                                                    true
#else
                                                    false
#endif
                                                    ));
  modules_.push_back(std::make_unique<SimpleModule>("metrics",
#ifdef MIG_ENABLE_METRICS
                                                    true
#else
                                                    false
#endif
                                                    ));
  modules_.push_back(std::make_unique<SimpleModule>("snapshot",
#ifdef MIG_ENABLE_SNAPSHOT
                                                    true
#else
                                                    false
#endif
                                                    ));
  modules_.push_back(std::make_unique<SimpleModule>("resume",
#ifdef MIG_ENABLE_RESUME
                                                    true
#else
                                                    false
#endif
                                                    ));
}

ModuleManager& ModuleManager::instance() {
  static ModuleManager mgr;
  return mgr;
}

const IModule* ModuleManager::find(const std::string& name) const {
  for (const auto& m : modules_)
    if (m->name() == name) return m.get();
  return nullptr;
}

std::vector<const IModule*> ModuleManager::all() const {
  std::vector<const IModule*> out;
  out.reserve(modules_.size());
  for (const auto& m : modules_) out.push_back(m.get());
  return out;
}

bool ModuleManager::compiled(const std::string& name) const {
  const IModule* m = find(name);
  return m ? m->compiled() : true;
}

bool ModuleManager::enabled(const Options& o, const std::string& name) const {
  const IModule* m = find(name);
  return m ? m->enabled(o) : true;
}

}  // namespace mig

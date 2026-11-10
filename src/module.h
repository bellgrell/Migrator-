#pragma once

#include "migrator.h"

#include <memory>
#include <string>
#include <vector>

namespace mig {

class IModule {
 public:
  virtual ~IModule() = default;
  virtual std::string name() const = 0;
  virtual bool compiled() const = 0;
  virtual bool enabled(const Options& o) const = 0;
};

class ModuleManager {
 public:
  static ModuleManager& instance();
  const IModule* find(const std::string& name) const;
  std::vector<const IModule*> all() const;
  bool compiled(const std::string& name) const;
  bool enabled(const Options& o, const std::string& name) const;

 private:
  ModuleManager();
  std::vector<std::unique_ptr<IModule>> modules_;
};

}  // namespace mig

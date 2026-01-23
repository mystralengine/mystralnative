#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace mystral {
namespace js {

enum class ResolveMode {
    Import,
    Require
};

enum class ModuleFormat {
    ESM,
    CJS,
    JSON
};

struct ResolvedPath {
    std::string path;
    bool isBundle = false;
};

struct ResolvedModule {
    ResolvedPath resolved;
    ModuleFormat format = ModuleFormat::CJS;
};

class ModuleResolver {
public:
    explicit ModuleResolver(const std::string& rootDir);

    void setRootDir(const std::string& rootDir);

    bool resolve(const std::string& specifier,
                 const std::string& referrer,
                 ResolveMode mode,
                 ResolvedModule& out,
                 std::string& error);

    bool resolveResolvedPath(const std::string& resolvedPath,
                             ResolvedModule& out,
                             std::string& error);

    bool readFile(const ResolvedPath& path, std::string& out, std::string& error) const;

    std::string dirname(const std::string& path) const;
    std::string normalizeSpecifier(const std::string& specifier) const;
    bool usingBundle() const;

private:
    struct JsonValue {
        enum class Type {
            Null,
            Bool,
            Number,
            String,
            Object,
            Array
        };
        Type type = Type::Null;
        bool boolVal = false;
        double numberVal = 0.0;
        std::string stringVal;
        std::unordered_map<std::string, JsonValue> objectVal;
        std::vector<JsonValue> arrayVal;
    };

    struct PackageInfo {
        std::string rootPath;
        std::string name;
        std::string type;
        std::string main;
        bool hasExports = false;
        bool hasImports = false;
        JsonValue exportsValue;
        JsonValue importsValue;
    };

    std::string rootDir_;
    bool useBundle_ = false;
    mutable std::unordered_map<std::string, PackageInfo> packageCache_;

    bool resolvePath(const std::string& pathSpec,
                     const std::string& referrer,
                     ResolveMode mode,
                     ResolvedModule& out,
                     std::string& error);
    bool resolvePackage(const std::string& specifier,
                        const std::string& referrer,
                        ResolveMode mode,
                        ResolvedModule& out,
                        std::string& error);
    bool resolveImports(const std::string& specifier,
                        const std::string& referrer,
                        ResolveMode mode,
                        ResolvedModule& out,
                        std::string& error);

    bool resolveAsFile(const std::string& path,
                       ResolveMode mode,
                       ResolvedModule& out,
                       std::string& error);
    bool resolveAsDirectory(const std::string& path,
                            ResolveMode mode,
                            ResolvedModule& out,
                            std::string& error);
    bool resolvePackageMain(const PackageInfo& pkg,
                            ResolveMode mode,
                            ResolvedModule& out,
                            std::string& error);
    bool resolvePackageExports(const PackageInfo& pkg,
                               const std::string& subpath,
                               ResolveMode mode,
                               ResolvedModule& out,
                               std::string& error);

    bool loadPackageJson(const std::string& packageRoot,
                         PackageInfo& out,
                         std::string& error) const;
    bool findPackageRoot(const std::string& startDir,
                         const std::string& packageName,
                         std::string& outRoot) const;
    bool findNearestPackage(const std::string& startDir,
                            std::string& outRoot) const;

    ModuleFormat detectFormatForPath(const std::string& path) const;
    std::string detectPackageTypeForPath(const std::string& path) const;

    bool fileExists(const std::string& path) const;
    bool dirExists(const std::string& path) const;

    bool resolveExportsTarget(const JsonValue& exportsValue,
                              const std::string& subpath,
                              const std::vector<std::string>& conditions,
                              std::string& outTarget,
                              std::string& error) const;
    bool resolveConditionalTarget(const JsonValue& value,
                                  const std::vector<std::string>& conditions,
                                  std::string& outTarget,
                                  std::string& error) const;
    bool isSubpathKey(const std::string& key) const;
    bool applyExportsPattern(const std::string& pattern,
                             const std::string& subpath,
                             std::string& out) const;

    bool parseJson(const std::string& input, JsonValue& out, std::string& error) const;
    bool parseJsonValue(const std::string& input, size_t& pos, JsonValue& out, std::string& error) const;
    bool parseJsonObject(const std::string& input, size_t& pos, JsonValue& out, std::string& error) const;
    bool parseJsonArray(const std::string& input, size_t& pos, JsonValue& out, std::string& error) const;
    bool parseJsonString(const std::string& input, size_t& pos, std::string& out, std::string& error) const;
    void skipWhitespace(const std::string& input, size_t& pos) const;
    bool isNumberChar(char c) const;
};

}  // namespace js
}  // namespace mystral

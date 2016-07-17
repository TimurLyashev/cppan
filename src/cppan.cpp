/*
 * Copyright (c) 2016, Egor Pugin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *     3. Neither the name of the copyright holder nor the names of
 *        its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cppan.h"

#include <fstream>
#include <iostream>
#include <regex>

#include <boost/algorithm/string.hpp>

#include <curl/curl.h>
#include <curl/easy.h>

#ifdef WIN32
#include <windows.h>

#include <libarchive/archive.h>
#include <libarchive/archive_entry.h>
#else
#include <archive.h>
#include <archive_entry.h>
#endif

#include <common.h>
#include <context.h>
#include <property_tree.h>
#include <project_path.h>

#define CPPAN_LOCAL_DIR "cppan"

#define LOG(x) std::cout << x << "\n"

const String cmake_config_filename = "CMakeLists.txt";
const String cmake_helpers_filename = "CppanHelpers.cmake";

using MimeType = String;
using MimeTypes = std::set<MimeType>;

static const MimeTypes source_mime_types{
    "text/x-asm",
    "text/x-c",
    "text/x-c++",
    "text/plain",
    "text/html", // ?
    "text/tex", // ? file with many comments can be this
};

static const std::set<String> header_file_extensions{
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".h++",
    ".HPP",
};

static const std::set<String> source_file_extensions{
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".c++",
    ".CPP",
};

static const std::set<String> other_source_file_extensions{
    ".s",
    ".S",
    ".asm",
    ".ipp",
};

bool is_allowed_file_extension(const path &p)
{
    auto e = p.extension().string();
    auto check = [&e](const auto &src)
    {
        return std::any_of(src.begin(), src.end(), [&e](const auto &s)
        {
            return s == e;
        });
    };
    if (check(header_file_extensions) ||
        check(source_file_extensions) ||
        check(other_source_file_extensions))
        return true;
    return false;
}

bool is_valid_file_type(const MimeTypes &types, const path &p, const String &s, String *error = nullptr, bool check_ext = false)
{
    auto mime = s.substr(0, s.find(';'));
    bool ok = std::any_of(types.begin(), types.end(), [&mime](const auto &s)
    {
        return mime == s;
    });
    if (!ok && check_ext)
        ok = is_allowed_file_extension(p);
    if (!ok && error)
        *error = "not supported: " + p.string() + ", mime: " + mime;
    return ok;
}

bool is_valid_file_type(const MimeTypes &types, const path &p, String *error = nullptr, bool check_ext = false)
{
    auto fret = system_with_output("file -ib " + p.string());
    auto s = std::get<1>(fret);
    return is_valid_file_type(types, p, s, error, check_ext);
}

bool is_valid_source_mime_type(const path &p, String *error = nullptr)
{
    return is_valid_file_type(source_mime_types, p, error, true);
}

bool is_valid_source(const path &p)
{
    if (!p.has_extension())
        return false;
    auto e = p.extension().string();
    return source_file_extensions.find(e) != source_file_extensions.end();
}

void check_file_types(const Files &files, const path &root)
{
    if (files.empty())
        return;

    String errors;
    for (auto &file : files)
    {
        auto s = (root / file).string();
        if (!check_filename(s))
            errors += "File '" + s + "' has prohibited symbols\n";
    }
    if (!errors.empty())
        throw std::runtime_error("Project sources did not pass file checks:\n" + errors);

    auto fn = get_temp_filename();
    std::ofstream o(fn.string(), std::ios::binary | std::ios::out);
    if (!o)
        throw std::runtime_error("Cannot open file for writing: " + fn.string());
    auto cwd = fs::current_path();
    for (auto &file : files)
    {
        auto s = (cwd / root / file).string();
        std::replace(s.begin(), s.end(), '\\', '/');
        o << "file -ib " << s << "\n";
    }
    o.close();

    auto fn2 = get_temp_filename();
    system(("sh " + fn.string() + " > " + fn2.string()).c_str());
    fs::remove(fn);
    std::ifstream ifile(fn2.string());
    if (!ifile)
        throw std::runtime_error("Cannot open file for reading: " + fn2.string());
    std::vector<String> lines;
    String s;
    while (std::getline(ifile, s))
    {
        if (!s.empty())
            lines.push_back(s);
    }
    ifile.close();
    fs::remove(fn2);
    if (lines.size() != files.size())
        throw std::runtime_error("Error during file checking");

    int i = 0;
    for (auto &file : files)
    {
        String error;
        is_valid_file_type(source_mime_types, file, lines[i], &error, true);
        if (!error.empty())
            errors += error + "\n";
        i++;
    }
    if (!errors.empty())
        throw std::runtime_error("Project did not pass file checks:\n" + errors);
}

String repeat(const String &e, int n)
{
    String s;
    s.reserve(e.size() * n);
    for (int i = 0; i < n; i++)
        s += e;
    return s;
}

const String config_delimeter_short = repeat("#", 40);
const String config_delimeter = config_delimeter_short + config_delimeter_short;

path get_home_directory()
{
#ifdef WIN32
    auto home = getenv("USERPROFILE");
    if (!home)
        throw std::runtime_error("Cannot get user's home directory (%USERPROFILE%)");
#else
    auto home = getenv("HOME");
    if (!home)
        throw std::runtime_error("Cannot get user's home directory ($HOME)");
#endif
    return home;
}

path get_config_filename()
{
    return get_root_directory() / ".cppan";
}

path get_root_directory()
{
    return get_home_directory() / ".cppan";
}

void config_section_title(Context &ctx, const String &t)
{
    ctx.addLine(config_delimeter);
    ctx.addLine("#");
    ctx.addLine("# " + t);
    ctx.addLine("#");
    ctx.addLine(config_delimeter);
    ctx.addLine();
}

template <class T>
auto get_scalar(const YAML::Node &node, const String &key, const T &default_ = T())
{
    const auto &n = node[key];
    if (n.IsDefined())
    {
        if (!n.IsScalar())
            throw std::runtime_error("'" + key + "' should be a scalar");
        return n.as<T>();
    }
    return default_;
};

template <class F>
void get_scalar_f(const YAML::Node &node, const String &key, F &&f)
{
    const auto &n = node[key];
    if (n.IsDefined())
    {
        if (!n.IsScalar())
            throw std::runtime_error("'" + key + "' should be a scalar");
        f(n);
    }
};

template <class T>
auto get_sequence(const YAML::Node &node)
{
    std::vector<T> result;
    const auto &n = node;
    if (!n.IsDefined())
        return result;
    if (n.IsScalar())
        result.push_back(n.as<String>());
    else
    {
        if (!n.IsSequence())
            return result;
        for (const auto &v : n)
            result.push_back(v.as<String>());
    }
    return result;
};

template <class T>
auto get_sequence(const YAML::Node &node, const String &key, const T &default_ = T())
{
    const auto &n = node[key];
    if (n.IsDefined() && !(n.IsScalar() || n.IsSequence()))
        throw std::runtime_error("'" + key + "' should be a sequence");
    auto result = get_sequence<T>(n);
    if (!default_.empty())
        result.push_back(default_);
    return result;
};

template <class T>
auto get_sequence_set(const YAML::Node &node)
{
    auto vs = get_sequence<T>(node);
    return std::set<T>(vs.begin(), vs.end());
}

template <class T>
auto get_sequence_set(const YAML::Node &node, const String &key)
{
    auto vs = get_sequence<T>(node, key);
    return std::set<T>(vs.begin(), vs.end());
}

template <class T1, class T2>
auto get_sequence_set(const YAML::Node &node, const String &key)
{
    auto vs = get_sequence<T2>(node, key);
    return std::set<T1>(vs.begin(), vs.end());
}

template <class F>
void get_sequence_and_iterate(const YAML::Node &node, const String &key, F &&f)
{
    const auto &n = node[key];
    if (n.IsDefined())
    {
        if (!n.IsSequence())
            throw std::runtime_error("'" + key + "' should be a sequence");
        for (const auto &v : n)
            f(v);
    }
};

template <class F>
void get_map(const YAML::Node &node, const String &key, F &&f)
{
    const auto &n = node[key];
    if (n.IsDefined())
    {
        if (!n.IsMap())
            throw std::runtime_error("'" + key + "' should be a map");
        f(n);
    }
};

template <class F>
void get_map_and_iterate(const YAML::Node &node, const String &key, F &&f)
{
    const auto &n = node[key];
    if (n.IsDefined())
    {
        if (!n.IsMap())
            throw std::runtime_error("'" + key + "' should be a map");
        for (const auto &v : n)
            f(v);
    }
};

template <class T>
void get_string_map(const YAML::Node &node, const String &key, T &data)
{
    const auto &n = node[key];
    if (n.IsDefined())
    {
        if (!n.IsMap())
            throw std::runtime_error("'" + key + "' should be a map");
        for (const auto &v : n)
            data.emplace(v.first.as<String>(), v.second.as<String>());
    }
};

template <class F1, class F2, class F3>
void get_variety(const YAML::Node &node, const String &key, F1 &&f_scalar, F2 &&f_seq, F3 &&f_map)
{
    const auto &n = node[key];
    if (!n.IsDefined())
        return;
    switch (n.Type())
    {
    case YAML::NodeType::Scalar:
        f_scalar(n);
        break;
    case YAML::NodeType::Sequence:
        f_seq(n);
        break;
    case YAML::NodeType::Map:
        f_map(n);
        break;
    }
}

template <class F1, class F3>
void get_variety_and_iterate(const YAML::Node &node, F1 &&f_scalar, F3 &&f_map)
{
    const auto &n = node;
    if (!n.IsDefined())
        return;
    switch (n.Type())
    {
    case YAML::NodeType::Scalar:
        f_scalar(n);
        break;
    case YAML::NodeType::Sequence:
        for (const auto &v : n)
            f_scalar(v);
        break;
    case YAML::NodeType::Map:
        for (const auto &v : n)
            f_map(v);
        break;
    }
}

template <class F1, class F3>
void get_variety_and_iterate(const YAML::Node &node, const String &key, F1 &&f_scalar, F3 &&f_map)
{
    const auto &n = node[key];
    get_variety_and_iterate(n, std::forward<F1>(f_scalar), std::forward<F1>(f_map));
}

void get_config_insertion(const YAML::Node &n, const String &key, String &dst)
{
    dst = get_scalar<String>(n, key);
    if (!dst.empty())
        dst.resize(dst.size() - 1); // remove trailing \n
}

ptree url_post(const String &url, const ptree &data)
{
    ptree p;
    std::ostringstream oss;
    pt::write_json(oss, data
#if !defined(CPPAN_TEST)
        , false
#endif
        );
    std::istringstream iss(url_post(url, oss.str()));
    pt::read_json(iss, p);
    return p;
}

void BuildSystemConfigInsertions::get_config_insertions(const YAML::Node &n)
{
#define ADD_CFG_INSERTION(x) get_config_insertion(n, #x, x)
    ADD_CFG_INSERTION(pre_sources);
    ADD_CFG_INSERTION(post_sources);
    ADD_CFG_INSERTION(post_target);
    ADD_CFG_INSERTION(post_alias);
#undef ADD_CFG_INSERTION
}

PackageInfo::PackageInfo(const Dependency &d)
{
    dependency = std::make_unique<Dependency>(d);
    auto v = d.version.toAnyVersion();
    target_name = d.package.toString() + (v == "*" ? "" : ("-" + v));
    variable_name = d.package.toString() + "_" + (v == "*" ? "" : ("_" + v));
    std::replace(variable_name.begin(), variable_name.end(), '.', '_');
}

void Project::findSources(path p)
{
    p /= root_directory;

    for (auto i = sources.begin(); i != sources.end();)
    {
        if (fs::exists(p / *i))
        {
            files.insert(*i);
            sources.erase(i++);
            continue;
        }
        ++i;
    }

    if ((sources.empty() && files.empty()) && !empty)
        throw std::runtime_error("'files' must be populated");

    std::map<String, std::regex> rgxs;
    for (auto &e : sources)
        rgxs[e] = std::regex(e);

    for (auto &f : boost::make_iterator_range(fs::recursive_directory_iterator(p), {}))
    {
        if (!fs::is_regular_file(f))
            continue;

        String s = relative(f.path(), p).string();
        std::replace(s.begin(), s.end(), '\\', '/');

        for (auto &e : rgxs)
        {
            if (std::regex_match(s, e.second))
                files.insert(s);
        }
    }

    if (files.empty() && !empty)
        throw std::runtime_error("no files found");

    // disable on windows during testing
#ifndef WIN32
    check_file_types(files, root_directory);
#endif

    header_only = std::none_of(files.begin(), files.end(), is_valid_source);

    if (!license.empty())
    {
        if (!fs::exists(root_directory / license))
            throw std::runtime_error("License does not exists");
        if (fs::file_size(root_directory / license) > 512 * 1024)
            throw std::runtime_error("license is invalid (should be text/plain and less than 512 KB)");
        files.insert(license);
    }

    if (!root_directory.empty())
        fs::copy_file(cppan_filename, root_directory / cppan_filename, fs::copy_option::overwrite_if_exists);
    files.insert(cppan_filename);
}

bool Project::writeArchive(const String &filename) const
{
    bool result = true;
    auto a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    archive_write_open_filename(a, filename.c_str());
    for (auto &f : files)
    {
        auto fn = f.string();
        auto fn_real = root_directory / f;
        if (!fs::exists(fn_real))
        {
            result = false;
            continue;
        }
        auto sz = fs::file_size(fn_real);
        auto e = archive_entry_new();
        archive_entry_set_pathname(e, fn.c_str());
        archive_entry_set_size(e, sz);
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_write_header(a, e);
        auto fp = fopen(fn_real.string().c_str(), "rb");
        if (!fp)
        {
            archive_entry_free(e);
            result = false;
            continue;
        }
        char buff[8192];
        size_t len;
        len = fread(buff, 1, sizeof(buff), fp);
        while (len > 0)
        {
            archive_write_data(a, buff, len);
            len = fread(buff, 1, sizeof(buff), fp);
        }
        fclose(fp);
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return result;
}

Config::Config()
{
    storage_dir = get_root_directory() / "packages";

    // add some common types
    check_types.insert("size_t");
    check_types.insert("void *");
}

Config::Config(const path &p)
    : Config()
{
    auto old = fs::current_path();
    fs::current_path(p);
    load_current_config();
    fs::current_path(old);
}

Config Config::load_system_config()
{
    auto fn = CONFIG_ROOT "default";
    Config c;
    if (!fs::exists(fn))
        return c;
    c.load_common(fn);
    return c;
}

Config Config::load_user_config()
{
    auto fn = get_config_filename();
    if (!fs::exists(fn))
    {
        boost::system::error_code ec;
        fs::create_directories(fn.parent_path(), ec);
        if (ec)
            throw std::runtime_error(ec.message());
        Config c = load_system_config();
        c.save(fn);
        return c;
    }
    Config c = load_system_config();
    c.load_common(fn);
    return c;
}

void Config::load_current_config()
{
    load(fs::current_path() / CPPAN_FILENAME);
}

void Config::load_common(const path &p)
{
    auto root = YAML::LoadFile(p.string());
    load_common(root);
}

void Config::load_common(const YAML::Node &root)
{
#define EXTRACT_VAR(r, val, var, type) \
    do                                 \
    {                                  \
        auto &v = r[var];              \
        if (v.IsDefined())             \
            val = v.as<type>();        \
    } while (0)
#define EXTRACT(val, type) EXTRACT_VAR(root, val, #val, type)
#define EXTRACT_AUTO(val) EXTRACT(val, decltype(val))

    EXTRACT_AUTO(host);
    EXTRACT(storage_dir, String);
    EXTRACT(root_project, String);

    auto &p = root["proxy"];
    if (p.IsDefined())
    {
        if (!p.IsMap())
            throw std::runtime_error("'proxy' should be a map");
        EXTRACT_VAR(p, proxy.host, "host", String);
        EXTRACT_VAR(p, proxy.user, "user", String);
    }

    packages_dir_type = packages_dir_type_from_string(get_scalar<String>(root, "packages_dir", "user"));

    auto check = [&root](auto &a, auto &&str)
    {
        auto s = get_sequence<String>(root, str);
        a.insert(s.begin(), s.end());
    };

    check(check_functions, "check_function_exists");
    check(check_includes, "check_include_exists");
    check(check_types, "check_type_size");
    check(check_libraries, "check_library_exists");

    get_map_and_iterate(root, "check_symbol_exists", [this](const auto &root)
    {
        auto f = root.first.template as<String>();
        auto s = root.second.template as<String>();
        if (root.second.IsSequence())
            check_symbols[f] = get_sequence_set<String>(root.second);
        else if (root.second.IsScalar())
            check_symbols[f].insert(s);
        else
            throw std::runtime_error("Symbol headers should be a scalar or a set");
    });

    bs_insertions.get_config_insertions(root);
}

void Config::load(const path &p)
{
    auto root = YAML::LoadFile(p.string());
    load_common(root);

    auto set_project = [this, &p](auto &&project, auto &&name)
    {
        project.cppan_filename = p.filename().string();
        project.package = relative_name_to_absolute(name);
        projects.push_back(project);
    };

    const auto &prjs = root["projects"];
    if (prjs.IsDefined())
    {
        if (!prjs.IsMap())
            throw std::runtime_error("'projects' should be a map");
        for (auto &prj : prjs)
            set_project(load_project(prj.second), prj.first.as<String>());
    }
    else
        set_project(load_project(root), "");
}

Project Config::load_project(const YAML::Node &root)
{
    Project p;

#define GET_BOOL(b) p.b = root[#b].IsDefined()

    GET_BOOL(empty);

    GET_BOOL(shared_only);
    GET_BOOL(static_only);

    if (p.shared_only && p.static_only)
        throw std::runtime_error("Project cannot be static and shared simultaneously");

    p.license = get_scalar<String>(root, "license");

    get_scalar_f(root, "root_directory", [&p](const auto &n)
    {
        auto cp = fs::current_path();
        p.root_directory = n.template as<String>();
        if (cp / p.root_directory < cp)
            throw std::runtime_error("'root_directory' cannot be less than current: " + p.root_directory.string() + ", " + cp.string());
    });

    get_map(root, "include_directories", [&p](const auto &n)
    {
        p.include_directories.public_ = get_sequence_set<path, String>(n, "public");
        p.include_directories.private_ = get_sequence_set<path, String>(n, "private");
    });
    if (p.include_directories.public_.empty())
        p.include_directories.public_.insert("include");
    p.include_directories.public_.insert("${CMAKE_CURRENT_BINARY_DIR}");

    p.exclude_from_build = get_sequence_set<path, String>(root, "exclude_from_build");

    p.bs_insertions.get_config_insertions(root);

    get_map_and_iterate(root, "options", [&p](const auto &opt_level)
    {
        auto ol = opt_level.first.template as<String>();
        if (!(ol == "any" || ol == "static" || ol == "shared"))
            throw std::runtime_error("Wrong option level dicrective");
        if (!opt_level.second.IsMap())
            throw std::runtime_error("'" + ol + "' should be a map");

        auto &option = p.options[ol];
        const auto &defs = opt_level.second["definitions"];

        auto add_defs = [&option, &defs](const auto &s)
        {
            auto dd = get_sequence_set<String, String>(defs, s);
            for (auto &d : dd)
                option.definitions.insert({ s,d });
        };

        add_defs("public");
        add_defs("private");
        add_defs("interface");

        option.include_directories = get_sequence_set<String, String>(opt_level.second, "include_directories");
        option.link_directories = get_sequence_set<String, String>(opt_level.second, "link_directories");
        option.link_libraries = get_sequence_set<String, String>(opt_level.second, "link_libraries");
        option.global_definitions = get_sequence_set<String, String>(opt_level.second, "global_definitions");

        option.bs_insertions.get_config_insertions(opt_level.second);
    });

    get_variety(root, "dependencies",
        [this, &p](const YAML::Node &d)
    {
        Dependency dependency;
        dependency.package = relative_name_to_absolute(d.as<String>());
        p.dependencies[dependency.package.toString()] = dependency;
    },
        [this, &p](const auto &dall)
    {
        for (auto d : dall)
        {
            Dependency dependency;
            dependency.package = this->relative_name_to_absolute(d.template as<String>());
            p.dependencies[dependency.package.toString()] = dependency;
        }
    },
        [this, &p](const auto &dall)
    {
        auto get_dep = [this](auto &deps, const auto &d)
        {
            Dependency dependency;
            dependency.package = this->relative_name_to_absolute(d.first.template as<String>());
            if (d.second.IsScalar())
                dependency.version = d.second.template as<String>();
            else if (d.second.IsMap())
            {
                for (const auto &v : d.second)
                {
                    auto key = v.first.template as<String>();
                    if (key == "version")
                        dependency.version = v.second.template as<String>();
                    else if (key == "package_dir")
                        dependency.package_dir_type = packages_dir_type_from_string(v.second.template as<String>());
                    else if (key == "patches")
                    {
                        for (const auto &p : v.second)
                            dependency.patches.push_back(p.template as<String>());
                    }
                    else
                        throw std::runtime_error("Unknown key: " + key);
                }
            }
            else
                throw std::runtime_error("Dependency should be a scalar or a map");
            deps[dependency.package.toString()] = dependency;
        };

        Dependencies dependencies_private;

        get_map_and_iterate(dall, "private",
            [this, &p, &get_dep, &dependencies_private](const auto &d)
        {
            get_dep(dependencies_private, d);
        });
        get_map_and_iterate(dall, "public",
            [this, &p, &get_dep](const auto &d)
        {
            get_dep(p.dependencies, d);
        });

        for (auto &d : dependencies_private)
        {
            d.second.flags.set(pfPrivate);
            p.dependencies.insert(d);
        }

        if (p.dependencies.empty() && dependencies_private.empty())
        {
            for (auto d : dall)
            {
                get_dep(p.dependencies, d);
            }
        }
    });

    auto read_sources = [&root](auto &a, const String &key, bool required = true)
    {
        auto files = root[key];
        if (!files.IsDefined())
            return;
        if (files.IsScalar())
        {
            a.insert(files.as<String>());
        }
        else if (files.IsSequence())
        {
            for (const auto &v : files)
                a.insert(v.as<String>());
        }
        else if (files.IsMap())
        {
            for (const auto &group : files)
            {
                if (group.second.IsScalar())
                    throw std::runtime_error("group '" + group.first.as<String>() + "' cannot be a scalar");
                else if (group.second.IsSequence())
                {
                    for (const auto &v : group.second)
                        a.insert(v.as<String>());
                }
                else if (group.second.IsMap())
                {
                    path root = get_scalar<String>(group.second, "root");
                    auto v = get_sequence<String>(group.second, "files");
                    for (auto &e : v)
                        a.insert((root / e).string());
                }
            }
        }
    };

    read_sources(p.sources, "files");
    read_sources(p.build_files, "build");

    return p;
}

ProjectPath Config::relative_name_to_absolute(const String &name)
{
    ProjectPath package;
    if (name.empty())
        return package;
    if (ProjectPath(name).is_relative())
    {
        if (root_project.empty())
            throw std::runtime_error("You're using relative names, but 'root_project' is missing");
        package = root_project / name;
    }
    else
        package = name;
    return package;
}

void Config::save(const path &p) const
{
#define EMIT_KV(k, v) \
    do { \
        e << YAML::Key << k; \
        e << YAML::Value << v; \
    } while (0)
#define EMIT_KV_SAME(k) EMIT_KV(#k, k)

    std::ofstream o(p.string());
    if (!o)
        throw std::runtime_error("Cannot open file: " + p.string());
    YAML::Emitter e(o);
    e.SetIndent(4);
    e << YAML::BeginMap;
    EMIT_KV_SAME(host);
    EMIT_KV("storage_dir", storage_dir.string());
    e << YAML::EndMap;
}

void Config::download_dependencies()
{
    // send request
    auto url = host;
    ptree data;
    for (auto &p : projects)
    {
        for (auto &d : p.dependencies)
        {
            if (d.second.package.is_relative())
                continue;
            ptree version;
            version.put("version", d.second.version.toString());
            data.put_child(ptree::path_type(d.second.package.toString(), '|'), version);
        }
    }
    // no deps found
    if (data.empty())
        return;

    LOG("Requesting dependency list");
    dependency_tree = url_post(url + "/api/find_dependencies", data);

    // read deps urls, download them, unpack
    int api = 0;
    if (dependency_tree.find("api") != dependency_tree.not_found())
        api = dependency_tree.get<int>("api");

    auto e = dependency_tree.find("error");
    if (e != dependency_tree.not_found())
        throw std::runtime_error(e->second.get_value<String>());

    if (api == 0)
        throw std::runtime_error("Api version is missing in the response");
    if (api != 1)
        throw std::runtime_error("Bad api version");

    String data_url = "data";
    if (dependency_tree.find("data_dir") != dependency_tree.not_found())
        data_url = dependency_tree.get<String>("data_dir");
    auto &remote_packages = dependency_tree.get_child("packages");
    for (auto &v : remote_packages)
    {
        Dependency dep;
        dep.package = v.first;
        dep.version = v.second.get<String>("version");
        dep.flags = decltype(dep.flags)(v.second.get<uint64_t>("flags"));
        dep.md5 = v.second.get<String>("md5");

        if (v.second.find("dependencies") != v.second.not_found())
        {
            std::set<int> idx;
            for (auto &tree_dep : v.second.get_child("dependencies"))
                idx.insert(tree_dep.second.get_value<int>());
            for (auto &v : remote_packages)
            {
                auto id = v.second.get<int>("id");
                if (idx.find(id) == idx.end())
                    continue;
                Dependency dep2;
                dep2.package = v.first;
                dep2.version = v.second.get<String>("version");
                dep2.flags = decltype(dep2.flags)(v.second.get<uint64_t>("flags"));
                dep.dependencies[dep2.package.toString()] = dep2;
            }
        }

        path dir;
        if (dep.flags[pfDirectDependency])
        {
            bool found = false;
            for (auto &p : projects)
            {
                auto i = p.dependencies.find(dep.package.toString());
                if (i == p.dependencies.end())
                {
                    for (auto &d : p.dependencies)
                    {
                        std::regex r(d.second.package.toString() + ".*");
                        if (std::regex_match(dep.package.toString(), r))
                        {
                            found = true;
                            d.second.version = dep.version;
                            dir = get_packages_dir(d.second.get_package_dir_type(packages_dir_type)) / dep.package.toString();
                            break;
                        }
                    }
                    if (!found)
                        continue;
                }
                else
                {
                    found = true;
                    auto &d = i->second;
                    d.version = dep.version;
                    dir = get_packages_dir(d.get_package_dir_type(packages_dir_type)) / d.package.toString();
                }
            }
            if (!found)
            {
                // actually should not happen
                throw std::logic_error("Internal error: cannot match received dependency");
                //dir = storage_dir / dep.package.toString();
            }
        }
        else
        {
            dir = storage_dir / dep.package.toString();
        }

        path version_dir = dir / dep.version.toString();
        dep.package_dir = version_dir;

        constexpr auto md5_filename = "archive.md5";
        auto md5file = dep.package_dir.parent_path() / md5_filename;

        // store md5 of archive
        bool must_download = false;
        {
            std::ifstream ifile(md5file.string());
            String file_md5;
            if (ifile)
            {
                ifile >> file_md5;
                ifile.close();
            }
            if (file_md5 != dep.md5 || dep.md5.empty() || file_md5.empty())
                must_download = true;
        }

        if (!fs::exists(version_dir) || must_download)
        {
            if (fs::exists(version_dir))
                fs::remove_all(version_dir);

            auto fs_path = ProjectPath(dep.package).toFileSystemPath().string();
            std::replace(fs_path.begin(), fs_path.end(), '\\', '/');
            String package_url = url + "/" + data_url + "/" + fs_path + "/" + dep.version.toString() + ".tar.gz";
            path fn = version_dir.string() + ".tar.gz";

            String dl_md5;
            DownloadData dd;
            dd.url = package_url;
            dd.fn = fn;
            dd.dl_md5 = &dl_md5;
            LOG("Downloading: " << dep.package.toString() << "-" << dep.version.toString());
            download_file(dd);

            if (dl_md5 != dep.md5)
                throw std::runtime_error("md5 does not match for package '" + dep.package.toString() + "'");

            std::ofstream ofile(md5file.string());
            if (ofile)
            {
                ofile << dep.md5;
                ofile.close();
            }
            else
                throw std::runtime_error("Cannot open the file '" + md5file.string() + "'");

            LOG("Unpacking: " << fn.string());
            try
            {
                unpack_file(fn, version_dir);
            }
            catch (...)
            {
                fs::remove_all(version_dir);
                throw;
            }
            fs::remove(fn);
        }

        auto config_file = version_dir / cmake_config_filename;
        std::ofstream ofile(config_file.string());
        if (!ofile)
            throw std::runtime_error("Cannot create a file: " + config_file.string());

        Config c(dep.package_dir);
        auto pi = c.print_package_config_file(ofile, dep, *this);
        if (dep.flags[pfDirectDependency])
            packages[pi.dependency->package.toString()] = pi;
        else
            indirect_dependencies[dep.package.toString()] = dep;
    }
}

PackageInfo Config::print_package_config_file(std::ofstream &o, const Dependency &d, Config &parent) const
{
    PackageInfo pi(d);
    bool header_only = pi.dependency->flags[pfHeaderOnly];

    const Project *pp = &projects[0];
    if (projects.size() > 1)
    {
        auto it_project = std::find_if(projects.begin(), projects.end(), [p2 = d.package](const auto &p)
        {
            return p.package == p2;
        });
        if (it_project == projects.end())
            throw std::runtime_error("No such project '" + d.package.toString() + "' in dependencies list");

        pp = &*it_project;
    }
    auto &p = *pp;

    // gather checks
#define GATHER_CHECK(c) parent.c.insert(c.begin(), c.end())
    GATHER_CHECK(check_functions);
    GATHER_CHECK(check_includes);
    GATHER_CHECK(check_types);
    GATHER_CHECK(check_symbols);
    GATHER_CHECK(check_libraries);
#undef GATHER_CHECK

    Context ctx;
    ctx.addLine("#");
    ctx.addLine("# cppan");
    ctx.addLine("# package: " + d.package.toString());
    ctx.addLine("# version: " + d.version.toString());
    ctx.addLine("#");
    ctx.addLine();

    // settings
    config_section_title(ctx, "settings");
    ctx.addLine("set(LIBRARY_TYPE STATIC)");
    ctx.addLine();
    ctx.addLine("if (\"${CPPAN_BUILD_SHARED_LIBS}\" STREQUAL \"ON\")");
    ctx.increaseIndent();
    ctx.addLine("set(LIBRARY_TYPE SHARED)");
    ctx.decreaseIndent();
    ctx.addLine("endif()");
    ctx.addLine();
    ctx.addLine("if (LIBRARY_TYPE_" + pi.variable_name + ")");
    ctx.increaseIndent();
    ctx.addLine("set(LIBRARY_TYPE ${LIBRARY_TYPE_" + pi.variable_name + "})");
    ctx.decreaseIndent();
    ctx.addLine("endif()");

    if (p.static_only)
        ctx.addLine("set(LIBRARY_TYPE STATIC)");
    else if (p.shared_only)
        ctx.addLine("set(LIBRARY_TYPE SHARED)");

    ctx.emptyLines(1);

    auto print_bs_insertion = [this, &p, &ctx](const String &name, const String BuildSystemConfigInsertions::*i)
    {
        config_section_title(ctx, name);
        if (projects.size() > 1)
        {
            ctx.addLine(bs_insertions.*i);
            ctx.emptyLines(1);
        }
        ctx.addLine(p.bs_insertions.*i);
        ctx.emptyLines(1);

        for (auto &ol : p.options)
        {
            auto &s = ol.second.bs_insertions.*i;
            if (s.empty())
                continue;

            if (ol.first == "any")
            {
                ctx.addLine(s);
            }
            else
            {
                ctx.addLine("if (LIBRARY_TYPE STREQUAL \"" + boost::algorithm::to_upper_copy(ol.first) + "\")");
                ctx.increaseIndent();
                ctx.addLine(s);
                ctx.decreaseIndent();
                ctx.addLine("endif()");
                ctx.emptyLines(1);
            }
        }

        ctx.emptyLines(1);
    };

    print_bs_insertion("pre sources", &BuildSystemConfigInsertions::pre_sources);

    // sources
    if (!header_only)
    {
        config_section_title(ctx, "sources");
        if (p.build_files.empty())
            ctx.addLine("file(GLOB_RECURSE src \"*\")");
        else
        {
            ctx.addLine("set(src");
            ctx.increaseIndent();
            for (auto &f : p.build_files)
            {
                auto s = f;
                std::replace(s.begin(), s.end(), '\\', '/');
                ctx.addLine("${CMAKE_CURRENT_SOURCE_DIR}/" + s);
            }
            ctx.decreaseIndent();
            ctx.addLine(")");
        }
        ctx.addLine();
    }

    // exclude files
    if (!p.exclude_from_build.empty())
    {
        config_section_title(ctx, "exclude files");
        for (auto &f : p.exclude_from_build)
            ctx << "list(REMOVE_ITEM src \"${CMAKE_CURRENT_SOURCE_DIR}/" << f.string() << "\")" << Context::eol;
        ctx.emptyLines(1);
    }

    print_bs_insertion("post sources", &BuildSystemConfigInsertions::post_sources);

    for (auto &ol : p.options)
        for (auto &ll : ol.second.link_directories)
            ctx.addLine("link_directories(" + ll + ")");
    ctx.emptyLines(1);

    // target
    config_section_title(ctx, "target: " + pi.target_name);
    if (d.flags[pfExecutable])
    {
        ctx << "add_executable                (" << pi.target_name << " ${src})" << Context::eol;
    }
    else
    {
        if (header_only)
            ctx << "add_library                   (" << pi.target_name << " INTERFACE)" << Context::eol;
        else
            ctx << "add_library                   (" << pi.target_name << " ${LIBRARY_TYPE} ${src})" << Context::eol;
    }

    // includes
    if (!p.include_directories.empty())
    {
        ctx << "target_include_directories    (" << pi.target_name << Context::eol;
        ctx.increaseIndent();
        if (header_only)
        {
            for (auto &idir : p.include_directories.public_)
                ctx.addLine("INTERFACE " + idir.string());
        }
        else
        {
            for (auto &idir : p.include_directories.public_)
                ctx.addLine("PUBLIC " + idir.string());
            for (auto &idir : p.include_directories.private_)
                ctx.addLine("PRIVATE " + idir.string());
        }
        ctx.decreaseIndent();
        ctx.addLine(")");
    }

    // deps
    ctx.addLine("target_link_libraries         (" + pi.target_name);
    ctx.increaseIndent();
    ctx.addLine((!header_only ? "PUBLIC" : "INTERFACE") + String(" cppan-helpers"));
    for (auto &d1 : p.dependencies)
    {
        auto iter = d.dependencies.find(d1.first);
        if (iter != d.dependencies.end() && iter->second.flags[pfExecutable])
            continue;
        PackageInfo pi1(d1.second);
        if (header_only)
            ctx.addLine("INTERFACE " + pi1.target_name);
        else
        {
            if (d1.second.flags[pfPrivate])
                ctx.addLine("PRIVATE " + pi1.target_name);
            else
                ctx.addLine("PUBLIC " + pi1.target_name);
        }
    }
    ctx.decreaseIndent();
    ctx.addLine(")");

    // solution folder
    if (!header_only)
    {
        ctx << "set_target_properties         (" << pi.target_name << " PROPERTIES" << Context::eol;
        ctx << "    FOLDER \"cppan/" << d.package.toString() << "/" << d.version.toString() << "\"" << Context::eol;
        ctx << ")" << Context::eol;
        ctx.emptyLines(1);
    }

    // defs
    for (auto &ol : p.options)
    {
        ctx.emptyLines(1);

        auto print_defs = [header_only, &ctx, &pi](const auto &ol)
        {
            if (ol.second.definitions.empty())
                return;
            ctx << "target_compile_definitions    (" << pi.target_name << Context::eol;
            ctx.increaseIndent();
            for (auto &def : ol.second.definitions)
            {
                if (header_only)
                    ctx << "INTERFACE " << def.second << Context::eol;
                else
                    ctx << boost::algorithm::to_upper_copy(def.first) << " " << def.second << Context::eol;
            }
            ctx.decreaseIndent();
            ctx.addLine(")");
        };
        auto print_set = [header_only, &ctx, &pi](const auto &a, const auto &s)
        {
            if (a.empty())
                return;
            ctx << s << "(" << pi.target_name << Context::eol;
            ctx.increaseIndent();
            for (auto &def : a)
            {
                if (header_only)
                    ctx << "INTERFACE ";
                else
                    ctx << "PUBLIC ";
                ctx << def << Context::eol;
            }
            ctx.decreaseIndent();
            ctx.addLine(")");
            ctx.addLine();
        };
        auto print_options = [&ol, &print_defs, &print_set]
        {
            print_defs(ol);
            print_set(ol.second.include_directories, "target_include_directories");
            print_set(ol.second.link_libraries, "target_link_libraries");
        };

        if (ol.first == "any")
        {
            print_options();
        }
        else
        {
            ctx.addLine("if (LIBRARY_TYPE STREQUAL \"" + boost::algorithm::to_upper_copy(ol.first) + "\")");
            print_options();
            ctx.addLine("endif()");
        }

        if (!ol.second.global_definitions.empty())
            parent.global_options[ol.first].global_definitions.insert(ol.second.global_definitions.begin(), ol.second.global_definitions.end());
    }

    ctx.emptyLines(1);
    ctx.addLine("set(lib " + pi.target_name + ")");
    ctx.addLine("set(target " + pi.target_name + ")");

    ctx.emptyLines(1);

    print_bs_insertion("post target", &BuildSystemConfigInsertions::post_target);

    // aliases
    Version ver = pi.dependency->version;
    if (!ver.isBranch())
    {
        config_section_title(ctx, "aliases");
        ver.patch = -1;
        ctx << "add_library(" << pi.dependency->package.toString() + "-" + ver.toAnyVersion() << " ALIAS " << pi.target_name << ")" << Context::eol;
        ver.minor = -1;
        ctx << "add_library(" << pi.dependency->package.toString() + "-" + ver.toAnyVersion() << " ALIAS " << pi.target_name << ")" << Context::eol;
        ctx << "add_library(" << pi.dependency->package.toString() << " ALIAS " << pi.target_name << ")" << Context::eol;
        ctx.addLine();
    }

    // export
    config_section_title(ctx, "export");
    ctx.addLine("export(TARGETS " + pi.target_name + " APPEND FILE ${CMAKE_BINARY_DIR}/cppan.cmake)");

    ctx.emptyLines(1);

    print_bs_insertion("post alias", &BuildSystemConfigInsertions::post_alias);

    // eof
    ctx.addLine(config_delimeter);
    ctx.addLine();

    ctx.splitLines();
    o << ctx.getText();

    return pi;
}

void Config::print_meta_config_file() const
{
    auto fn = fs::current_path() / CPPAN_LOCAL_DIR / cmake_config_filename;
    fs::create_directories(fn.parent_path());
    std::ofstream o(fn.string());
    if (!o)
        throw std::runtime_error("Cannot create a file: " + fn.string());

    Context ctx;
    ctx.addLine("#");
    ctx.addLine("# cppan");
    ctx.addLine("# meta config file");
    ctx.addLine("#");
    ctx.addLine();
    ctx.addLine("cmake_minimum_required(VERSION 3.0.0)");
    ctx.addLine();

    ctx.addLine("include(" + cmake_helpers_filename + ")");
    ctx.addLine();

    config_section_title(ctx, "variables");
    ctx.addLine("set(USES_CPPAN 1 CACHE STRING \"CPPAN is turned on\")");
    ctx.addLine();
    ctx.addLine("set(CPPAN_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})");
    ctx.addLine("set(CPPAN_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})");
    ctx.addLine();
    ctx.addLine("set(CMAKE_POSITION_INDEPENDENT_CODE ON)");
    ctx.addLine();

    /*o << "if (NOT CPPAN_LIBRARY_TYPE)\n";
    o << "    set(CPPAN_LIBRARY_TYPE STATIC)\n";
    o << "endif(NOT CPPAN_LIBRARY_TYPE)" << "\n";

    o << config_section_title("library types");
    o << "set(LIBRARY_TYPE_cppan ${CPPAN_LIBRARY_TYPE})" << "\n";
    o << "\n";
    for (auto &p : packages)
    {
        PackageInfo pi(p.second);
        o << "set(LIBRARY_TYPE_" << pi.variable_name << " ${CPPAN_LIBRARY_TYPE})" << "\n";
    }
    o << "\n";
    for (auto &id : indirect_dependencies)
    {
        PackageInfo pi(id.second);
        //o << "set(LIBRARY_TYPE_" << pi.variable_name << " ${CPPAN_LIBRARY_TYPE})" << "\n";
    }
    o << "\n";*/

    auto add_dep_subdir = [&ctx](const auto &package_dir)
    {
        auto src_dir = package_dir.string();
        boost::algorithm::replace_all(src_dir, "\\", "/");
        auto bin_dir = (package_dir.parent_path().filename() / package_dir.filename()).string();
        boost::algorithm::replace_all(bin_dir, "\\", "/");
        bin_dir = sha1(bin_dir).substr(0, 6);
        ctx << "add_subdirectory(" << src_dir << " " << bin_dir << ")" << Context::eol;
    };

    if (!packages.empty())
    {
        config_section_title(ctx, "direct dependencies");
        for (auto &p : packages)
            add_dep_subdir(p.second.dependency->package_dir);
        ctx.addLine();
    }

    if (!indirect_dependencies.empty())
    {
        config_section_title(ctx, "indirect dependencies");
        for (auto &id : indirect_dependencies)
            add_dep_subdir(id.second.package_dir);
        ctx.addLine();
    }

    ProjectFlags flags;
    flags[pfExecutable] = true;
    auto ulll = flags.to_ullong();

    const String cppan_project_name = "cppan";
    config_section_title(ctx, "main library");
    ctx.addLine("add_library                   (" + cppan_project_name + " INTERFACE)");
    if (!packages.empty())
    {
        ctx.addLine("target_link_libraries         (" + cppan_project_name);
        ctx.increaseIndent();
        for (auto &p : packages)
        {
            if (p.second.dependency->flags[pfExecutable])
                continue;
            ctx.addLine("INTERFACE " + p.second.target_name);
        }
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.addLine();
    }
    ctx.addLine("export(TARGETS " + cppan_project_name + " APPEND FILE ${CMAKE_BINARY_DIR}/cppan.cmake)");

    ctx.emptyLines(1);
    ctx.addLine(config_delimeter);
    ctx.addLine();

    o << ctx.getText();
}

void Config::print_helper_file() const
{
    auto fn = fs::current_path() / CPPAN_LOCAL_DIR / cmake_helpers_filename;
    fs::create_directories(fn.parent_path());
    std::ofstream o(fn.string());
    if (!o)
        throw std::runtime_error("Cannot create a file: " + fn.string());

    Context ctx;
    ctx.addLine("#");
    ctx.addLine("# cppan");
    ctx.addLine("# helper routines");
    ctx.addLine("#");
    ctx.addLine();

    config_section_title(ctx, "cmake setup");
    ctx.addLine(R"(# Use solution folders.
set_property(GLOBAL PROPERTY USE_FOLDERS ON))");
    ctx.addLine();

    // cmake includes
    config_section_title(ctx, "cmake includes");
    ctx.addLine(R"(include(CheckCXXSymbolExists)
include(CheckFunctionExists)
include(CheckIncludeFiles)
include(CheckLibraryExists)
include(CheckTypeSize)
include(TestBigEndian))");
    ctx.addLine();

    config_section_title(ctx, "common checks");

    ctx.addLine("test_big_endian(WORDS_BIGENDIAN)");
    // aliases
    ctx.addLine("set(BIG_ENDIAN ${WORDS_BIGENDIAN} CACHE STRING \"endianness alias\")");
    ctx.addLine("set(BIGENDIAN ${WORDS_BIGENDIAN} CACHE STRING \"endianness alias\")");
    ctx.addLine("set(HOST_BIG_ENDIAN ${WORDS_BIGENDIAN} CACHE STRING \"endianness alias\")");
    ctx.addLine();

    // checks
    config_section_title(ctx, "checks");

    auto convert_function = [](const auto &s)
    {
        return "HAVE_" + boost::algorithm::to_upper_copy(s);
    };
    auto convert_include = [](const auto &s)
    {
        auto v_def = "HAVE_" + boost::algorithm::to_upper_copy(s);
        for (auto &c : v_def)
        {
            if (!isalnum(c))
                c = '_';
        }
        return v_def;
    };
    auto convert_type = [](const auto &s, const std::string &prefix = "HAVE_")
    {
        String v_def = prefix;
        v_def += boost::algorithm::to_upper_copy(s);
        for (auto &c : v_def)
        {
            if (c == '*')
                c = 'P';
            else if (!isalnum(c))
                c = '_';
        }
        return v_def;
    };

    auto add_checks = [&ctx](const auto &a, const String &s, auto &&f)
    {
        for (auto &v : a)
            ctx.addLine(s + "(\"" + v + "\" " + f(v) + ")");
        ctx.emptyLines(1);
    };
    auto add_symbol_checks = [&ctx](const auto &a, const String &s, auto &&f)
    {
        for (auto &v : a)
        {
            ctx << s + "(\"" + v.first + "\" \"";
            for (auto &h : v.second)
                ctx << h << ";";
            ctx << "\" " << f(v.first) << ")" << Context::eol;
        }
        ctx.emptyLines(1);
    };
    auto add_if_definition = [&ctx](const String &s, auto &&... defs)
    {
        auto print_def = [&ctx](auto &&s)
        {
            ctx << "INTERFACE " << s << Context::eol;
            return 0;
        };
        ctx.addLine("if (" + s + ")");
        ctx.increaseIndent();
        ctx << "target_compile_definitions(cppan-helpers" << Context::eol;
        ctx.increaseIndent();
        print_def(s);
        using expand_type = int[];
        expand_type{ 0, print_def(std::forward<decltype(defs)>(defs))... };
        ctx.decreaseIndent();
        ctx.addLine(")");
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();
    };
    auto add_check_definitions = [&ctx, &add_if_definition](const auto &a, auto &&f)
    {
        for (auto &v : a)
            add_if_definition(f(v));
    };
    auto add_check_symbol_definitions = [&ctx, &add_if_definition](const auto &a, auto &&f)
    {
        for (auto &v : a)
            add_if_definition(f(v.first));
    };

    add_checks(check_functions, "check_function_exists", convert_function);
    add_symbol_checks(check_symbols, "check_cxx_symbol_exists", convert_function);
    add_checks(check_includes, "check_include_files", convert_include);
    add_checks(check_types, "check_type_size", convert_type);

    for (auto &v : check_types)
    {
        ctx.addLine("if (" + convert_type(v) + ")");
        ctx.increaseIndent();
        ctx.addLine("set(" + convert_type(v, "SIZE_OF_") + " ${" + convert_type(v) + "})");
        ctx.addLine("set(" + convert_type(v, "SIZEOF_")  + " ${" + convert_type(v) + "})");
        ctx.decreaseIndent();
        ctx.addLine("endif()");
        ctx.addLine();
    }

    // fixups
    // put bug workarounds here
    //config_section_title(ctx, "fixups");
    ctx.emptyLines(1);

    // library
    config_section_title(ctx, "library");

    ctx.addLine("add_library(cppan-helpers INTERFACE)");
    ctx.addLine();

    // common definitions
    ctx << "target_compile_definitions(cppan-helpers" << Context::eol;
    ctx.increaseIndent();
    ctx.addLine("INTERFACE CPPAN"); // build is performed under CPPAN
    ctx.decreaseIndent();
    ctx.addLine(")");
    ctx.addLine();

    // common link libraries
    ctx.addLine(R"(if (WIN32)
target_link_libraries(cppan-helpers
    INTERFACE Ws2_32
)
else()
target_link_libraries(cppan-helpers
    INTERFACE pthread
)
endif()
)");
    ctx.addLine();

    // Do not use APPEND here. It's the first file that will clear cppan.cmake.
    ctx.addLine("export(TARGETS cppan-helpers FILE ${CMAKE_BINARY_DIR}/cppan.cmake)");
    ctx.emptyLines(1);

    // global definitions
    config_section_title(ctx, "global definitions");

    ctx << "target_compile_definitions(cppan-helpers" << Context::eol;
    ctx.increaseIndent();
    for (auto &o : global_options)
    {
        for (auto &opt : o.second.global_definitions)
            ctx.addLine("INTERFACE " + opt);
    }
    ctx.decreaseIndent();
    ctx.addLine(")");
    ctx.addLine();

    // definitions
    config_section_title(ctx, "definitions");

    add_if_definition("WORDS_BIGENDIAN", "BIGENDIAN", "BIG_ENDIAN", "HOST_BIG_ENDIAN");

    add_check_definitions(check_functions, convert_function);
    add_check_symbol_definitions(check_symbols, convert_function);
    add_check_definitions(check_includes, convert_include);
    add_check_definitions(check_types, convert_type);

    // re-run cppan when root cppan.yml is changed
    config_section_title(ctx, "cppan regenerator");
    ctx.addLine(R"(add_custom_target(run-cppan
    COMMAND cppan
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    DEPENDS ${PROJECT_SOURCE_DIR}/cppan.yml
    SOURCES ${PROJECT_SOURCE_DIR}/cppan.yml
)
set_target_properties(run-cppan PROPERTIES
    FOLDER "cppan"
))");
    ctx.addLine();

    ctx.addLine(config_delimeter);
    ctx.addLine();
    o << ctx.getText();
}

void Config::create_build_files() const
{
    print_meta_config_file();
    print_helper_file();
}

path Config::get_packages_dir(PackagesDirType type)
{
    switch (type)
    {
    case PackagesDirType::Local:
        return CPPAN_LOCAL_DIR;
    case PackagesDirType::User:
        return load_user_config().storage_dir;
    case PackagesDirType::System:
        return load_system_config().storage_dir;
    }
    return storage_dir;
}

PackagesDirType packages_dir_type_from_string(const String &s)
{
    if (s == "local")
        return PackagesDirType::Local;
    if (s == "user")
        return PackagesDirType::User;
    if (s == "system")
        return PackagesDirType::System;
    throw std::runtime_error("Unknown 'packages_dir'. Should be one of [local, user, system]");
}
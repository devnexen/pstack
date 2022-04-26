#include "libpstack/dwarf.h"
#include "libpstack/flags.h"
#include "libpstack/global.h"
#include "libpstack/proc.h"
#include "libpstack/fs.h"
#include "libpstack/ps_callback.h"
#if defined(WITH_PYTHON2) || defined(WITH_PYTHON3)
#define WITH_PYTHON
#include "libpstack/python.h"
#endif

#include <sys/types.h>
#include <sys/signal.h>

#include <sysexits.h>
#include <unistd.h>

#include <csignal>

#include <iostream>
#include <set>

#define XSTR(a) #a
#define STR(a) XSTR(a)
extern std::ostream & operator << (std::ostream &os, const JSON<ThreadStack, Process *> &jt);

namespace {
bool doJson = false;
volatile bool interrupted = false;

std::ostream &
pstack(Process &proc, std::ostream &os, const PstackOptions &options, int maxFrames)
{
   const auto &threadStacks = proc.getStacks(options, maxFrames);

    if (doJson) {
        os << json(threadStacks, &proc);
    } else {
        os << "process: " << *proc.io << "\n";
        for (auto &s : threadStacks) {
            proc.dumpStackText(os, s, options);
            os << std::endl;
        }
    }
    return os;
}

#ifdef WITH_PYTHON
template<int V> void doPy(Process &proc, std::ostream &o,
      const PstackOptions &options, bool showModules, const PyInterpInfo &info) {
    StopProcess here(&proc);
    PythonPrinter<V> printer(proc, o, options, info);
    if (!printer.interpFound())
        throw Exception() << "no python interpreter found";
    printer.printInterpreters(showModules);
}

/**
 * @brief Given a process, tries to print the Python strack trace of it.
 * If the process wasn't a Python process, returns false.
 * True on successful printing of Python stack trace
 * 
 * @param proc          The process
 * @param o             The stream to which to print the otutput
 * @param options       Options
 * @param showModules   Whether to show modules
 * @return              boolean of whether the process was a Python process or not
 */
bool pystack(Process &proc, std::ostream &o, const PstackOptions &options, bool showModules) {
    PyInterpInfo info = getPyInterpInfo(proc);

    if (info.libpython == nullptr) // not a python process or python interpreter not found
        return false;

    if (info.versionHex < V2HEX(3, 0)) { // Python 2.x
#ifdef WITH_PYTHON2
        doPy<2>(proc, o, options, showModules, info);
#else
        throw (Exception() << "no support for discovered python 2 interpreter");
#endif
    } else { // Python 3.x
#ifdef WITH_PYTHON3
        doPy<3>(proc, o, options, showModules, info);
#else
        throw (Exception() << "no support for discovered python 3 interpreter");
#endif
    }
    return true;
}
#endif

int
usage(std::ostream &os, const char *name, const Flags &options)
{
     os <<
"usage: " << name << " <[ exe ] <PID | core> >+\n"
"\n"
"print a stack trace of PID or core. If specified, assume image was created from\n"
" execing `exe`, otherwise, the executable is inferred from the process or core\n"
"\n"
"available options:\n" << options <<  "\n";
     return EX_USAGE;
}

int
emain(int argc, char **argv)
{
    int maxFrames = 1024;
    Dwarf::ImageCache imageCache;
    double sleepTime = 0.0;
    PstackOptions options;

#if defined(WITH_PYTHON)
    bool doPython = false;
    bool pythonModules = false;
#endif
    std::vector<std::string> btLogs;
    std::string execName;
    bool printAllStacks = false;
    int exitCode = -1; // used for options that exit immediately to signal exit.

    Flags flags;
    flags
    .add("replace-path",
            'F',
            "from:to",
            "replace `from` with `to` in paths when finding shared libraries",
            [&](const char *arg) {
                auto sep = strchr(arg, ':');
                if (sep == 0)
                    usage(std::cerr, argv[0], flags);
                pathReplacements.push_back(std::make_pair(
                            std::string(arg, sep - arg), std::string(sep + 1))); })

    .add("debug-dir",
            'g',
            "directory",
            "extra location to find debug files for binaries and shared libraries",
            [&](const char *arg) { Elf::globalDebugDirectories.push_back(arg); })

    .add("constant",
            'b',
            "delay",
            "repeat pstack, with `delay` seconds between each iteration (can be non-integer)",
            Flags::set(sleepTime))

    .add("elf-dump",
            'd',
            "ELF file",
            "dump details of an ELF image in JSON and exit",
            [&](const char *arg) {
                std::cout << json(Elf::Object(imageCache, loadFile(arg)));
                exitCode = 0; })

    .add("dwarf-dump",
            'D',
            "ELF file",
            "dump details of DWARF information in an ELF image in JSON and exit",
            [&](const char *arg) {
                auto dumpobj = std::make_shared<Elf::Object>(imageCache, loadFile(arg));
                auto di = std::make_shared<Dwarf::Info>(dumpobj, imageCache);
                std::cout << json(*di);
                exitCode = 0; })

    .add("depth",
            'r',
            "depth",
            "max depth when printing python structures",
            Flags::set(options.maxdepth))

    .add("max-frames",
            'M',
            "max frames",
            "maximum number of stack frames to print for a thread",
            Flags::set(maxFrames))

    .add("help",
            'h',
            "generate this help message",
            [&]() { exitCode = usage(std::cout, argv[0], flags); })

    .add("args",
            'a',
            "attempt to show the value of arguments to functions",
            Flags::setf(options.doargs))

    .add("json",
            'j',
            "use JSON output rather than plaintext",
            Flags::setf(doJson))

    .add("no-src",
            's',
            "don't include source info",
            Flags::setf(options.nosrc))

    .add("verbose",
            'v',
            "more debugging data. Can be repeated",
            [&]() { ++verbose; })

    .add("no-threaddb",
            't',
            "don't use the thread_db functions to enumerate pthreads (just uses LWPs)",
            Flags::setf(options.nothreaddb))

    .add("all",
            'A',
            "show both python and DWARF (C/C++/go/rust) stack traces",
            Flags::setf(printAllStacks))

    .add("no-ext-debug",
            'n',
            "don't load external debugging information when processing",
            Flags::setf(Elf::noExtDebug))

    .add("version",
            'V',
            "dump version and exit",
            [&]() {
               std::clog << STR(VERSION) << "\n";
               exitCode = 0; })

#ifdef WITH_PYTHON
    .add("python-modules",
            'm',
            "print contents of all python modules when tracing",
            Flags::setf(pythonModules))

    .add("python",
            'p',
            "print python stack traces",
            Flags::setf(doPython))

    .add("locals",
            'l',
            "print local variables (just python for now)",
            Flags::setf(options.dolocals))
#endif
    .add("from-log",
            'L',
            "log-file",
            "print stack trace given log file including instruction pointers",
            [&](const char *log) {
               btLogs.push_back(log);
            })
    .add("executable",
          'e',
          "executable",
          "executable to use by default", [&](const char *opt) { execName = opt; })

    .parse(argc, argv);

    if (exitCode != -1)
        return exitCode;

    if (optind == argc && btLogs.empty())
        return usage(std::cerr, argv[0], flags);

    // any instance of a non-core ELF image will override default behaviour of
    // discovering the executable
    Elf::Object::sptr exec;
    if (execName != "")
       exec = imageCache.getImageForName(execName);


    auto doStack = [=, &options] (Process &proc) {
        proc.load(options);
        while (!interrupted) {
#if defined(WITH_PYTHON)
            if (doPython || printAllStacks) {
                bool isPythonProcess = pystack(proc, std::cout, options, pythonModules);
                // error if -p but not python process
                if (doPython && !isPythonProcess)
                    throw Exception() << "Couldn't find a Python interpreter";
            }
            if (!doPython)
#endif
            {
                pstack(proc, std::cout, options, maxFrames);
            }
            if (sleepTime != 0.0)
                usleep(sleepTime * 1000000);
            else
                break;
        }
    };

    if (!btLogs.empty()) {
       LogProcess lp{exec, btLogs, options, imageCache};
       doStack(lp);
       return 0;
    }

    for (int i = optind; i < argc; i++) {
        try {
            auto process = Process::load(exec, argv[i], options, imageCache);
            if (process == nullptr)
                exec = imageCache.getImageForName(argv[i]);
            else
                doStack(*process);
        } catch (const std::exception &e) {
            std::cerr << "trace of " << argv[i] << " failed: " << e.what() << "\n";
        }
    }
    return 0;
}
}

int
main(int argc, char **argv)
{
    try {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = [](int) { interrupted = true; };
        // Only interrupt cleanly once. Then just terminate, in case we're stuck in a loop
        sa.sa_flags = SA_RESETHAND;
        sigaction(SIGINT, &sa, nullptr);
        emain(argc, argv);
    }
    catch (std::exception &ex) {
        std::clog << "error: " << ex.what() << std::endl;
        return EX_SOFTWARE;
    }
}

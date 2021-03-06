// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fbl/array.h>
#include <fbl/unique_fd.h>
#include <fcntl.h>
#include <getopt.h>
#include <lib/fdio/io.h>
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/zx/vmo.h>
#include <unistd.h>
#include <utility>
#include <zircon/kernel-counters.h>
#include <zircon/status.h>

namespace {

constexpr char kShortOptions[] = "ltv";
constexpr struct option kLongOptions[] = {
    { "list", 0, nullptr, 'l' },
    { "terse", 0, nullptr, 't' },
    { "verbose", 0, nullptr, 'v' },
    { nullptr, 0, nullptr, 0 }
};

void usage(const char* myname) {
    printf("\
Usage: %s [-ltv] [--list] [--terse] [--verbose] [PREFIX...]\n\
Prints one counter per line.\n\
With --list or -l, show names and types rather than values.\n\
With --terse or -t, show only values and no names.\n\
With --verbose or -v, show space-separated lists of per-CPU values.\n\
Otherwise values are aggregated summaries across all CPUs.\n\
If PREFIX arguments are given, only matching names are shown.\n\
Results are always sorted by name.\n\
",
           myname);
};

constexpr char kVmoFileDir[] = "/boot/kernel";

}  // anonymous namespace

int main(int argc, char** argv) {
    bool list = false, terse = false, verbose = false;

    int opt;
    while ((opt = getopt_long(argc, argv, kShortOptions, kLongOptions,
                              nullptr)) != -1) {
        switch (opt) {
        case 'l':
            list = true;
            break;
        case 't':
            terse = true;
            break;
        case 'v':
            verbose = true;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (list + terse + verbose > 1) {
        fprintf(stderr,
                "%s: --list, --terse, and --verbose are mutually exclusive",
                argv[0]);
        usage(argv[0]);
        return 1;
    }

   fbl::unique_fd dir_fd(open(kVmoFileDir, O_RDONLY | O_DIRECTORY));
    if (!dir_fd) {
        fprintf(stderr, "%s: %s\n", kVmoFileDir, strerror(errno));
        return 2;
    }

    fzl::OwnedVmoMapper desc_mapper;
    const counters::DescriptorVmo* desc;
    {
        fbl::unique_fd desc_fd(
            openat(dir_fd.get(), counters::DescriptorVmo::kVmoName, O_RDONLY));
        if (!desc_fd) {
            fprintf(stderr, "%s/%s: %s\n",
                    kVmoFileDir, counters::DescriptorVmo::kVmoName,
                    strerror(errno));
            return 2;
        }
        zx::vmo vmo;
        zx_status_t status = fdio_get_vmo_exact(
            desc_fd.get(), vmo.reset_and_get_address());
        if (status != ZX_OK) {
            fprintf(stderr, "fdio_get_vmo_exact: %s: %s\n",
                    counters::DescriptorVmo::kVmoName,
                    zx_status_get_string(status));
            return 2;
        }
        uint64_t size;
        status = vmo.get_size(&size);
        if (status != ZX_OK) {
            fprintf(stderr, "cannot get %s VMO size: %s\n",
                    counters::DescriptorVmo::kVmoName,
                    zx_status_get_string(status));
            return 2;
        }
        status = desc_mapper.Map(std::move(vmo), size, ZX_VM_PERM_READ);
        if (status != ZX_OK) {
            fprintf(stderr, "cannot map %s VMO: %s\n",
                    counters::DescriptorVmo::kVmoName,
                    zx_status_get_string(status));
            return 2;
        }
        desc = reinterpret_cast<counters::DescriptorVmo*>(desc_mapper.start());
        if (desc->magic != counters::DescriptorVmo::kMagic) {
            fprintf(stderr,
                    "%s: magic number %" PRIu64 " != expected %" PRIu64 "\n",
                    counters::DescriptorVmo::kVmoName, desc->magic,
                    counters::DescriptorVmo::kMagic);
            return 2;
        }
        if (size < sizeof(*desc) + desc->descriptor_table_size) {
            fprintf(stderr, "%s size %#" PRIx64 " too small for %" PRIu64
                    " bytes of descriptor table\n",
                    counters::DescriptorVmo::kVmoName,
                    size, desc->descriptor_table_size);
            return 2;
        }
    }

    fzl::OwnedVmoMapper arena_mapper;
    const volatile int64_t* arena = nullptr;
    if (!list) {
        fbl::unique_fd arena_fd(
            openat(dir_fd.get(), counters::kArenaVmoName, O_RDONLY));
        if (!arena_fd) {
            fprintf(stderr, "%s/%s: %s\n",
                    kVmoFileDir, counters::kArenaVmoName, strerror(errno));
            return 2;
        }
        zx::vmo vmo;
        zx_status_t status = fdio_get_vmo_exact(
            arena_fd.get(), vmo.reset_and_get_address());
        if (status != ZX_OK) {
            fprintf(stderr, "fdio_get_vmo_exact: %s: %s\n",
                    counters::kArenaVmoName, zx_status_get_string(status));
            return 2;
        }
        uint64_t size;
        status = vmo.get_size(&size);
        if (status != ZX_OK) {
            fprintf(stderr, "cannot get %s VMO size: %s\n",
                    counters::kArenaVmoName, zx_status_get_string(status));
            return 2;
        }
        if (size < desc->max_cpus * desc->num_counters() * sizeof(int64_t)) {
            fprintf(stderr, "%s size %#" PRIx64 " too small for %" PRIu64
                    " CPUS * %" PRIu64 " counters\n",
                    counters::kArenaVmoName, size,
                    desc->max_cpus, desc->num_counters());
            return 2;
        }
        status = arena_mapper.Map(std::move(vmo), size, ZX_VM_PERM_READ);
        if (status != ZX_OK) {
            fprintf(stderr, "cannot map %s VMO: %s\n",
                    counters::kArenaVmoName, zx_status_get_string(status));
            return 2;
        }
        arena = reinterpret_cast<int64_t*>(arena_mapper.start());
    }

    dir_fd.reset();

    fbl::Array<bool> matched(new bool[argc - optind](), argc - optind);
    auto matches = [&](const char* name) -> bool {
        if (optind == argc) {
            return true;
        }
        for (int i = optind; i < argc; ++i) {
            if (!strncmp(name, argv[i], strlen(argv[i]))) {
                matched[i - optind] = true;
                return true;
            }
        }
        return false;
    };

    for (size_t i = 0; i < desc->num_counters(); ++i) {
        const auto& entry = desc->descriptor_table[i];
        if (matches(entry.name)) {
            if (list) {
                fputs(entry.name, stdout);
                switch (entry.type) {
                case counters::Type::kSum:
                    puts(" sum");
                    break;
                case counters::Type::kMin:
                    puts(" min");
                    break;
                case counters::Type::kMax:
                    puts(" max");
                    break;
                default:
                    printf(" ??? unknown type %" PRIu64 " ???\n",
                           static_cast<uint64_t>(entry.type));
                }
            } else {
                if (!terse) {
                    printf("%s =%s", entry.name,
                           !verbose ? " " :
                           entry.type == counters::Type::kMin ? " min(" :
                           entry.type == counters::Type::kMax ? " max(" :
                           " ");
                }
                int64_t value = 0;
                for (uint64_t cpu = 0; cpu < desc->max_cpus; ++cpu) {
                    const int64_t cpu_value =
                        arena[(cpu * desc->num_counters()) + i];
                    if (verbose) {
                        printf("%s%" PRId64,
                               cpu == 0 ? "" :
                               entry.type == counters::Type::kSum ? " + " :
                               ", ",
                               cpu_value);
                    }
                    switch (entry.type) {
                    case counters::Type::kSum:
                    default:
                        value += cpu_value;
                        break;
                    case counters::Type::kMin:
                        if (cpu_value < value) {
                            value = cpu_value;
                        }
                        break;
                    case counters::Type::kMax:
                        if (cpu_value > value) {
                            value = cpu_value;
                        }
                        break;
                    }
                }
                if (verbose) {
                    printf("%s = %" PRId64 "\n",
                           entry.type == counters::Type::kSum ? "" : ")",
                           value);
                } else {
                    printf("%" PRId64 "\n", value);
                }
            }
        }
    }

    // Check that each prefix was actually used.
    int status = 0;
    for (auto it = matched.begin(); it != matched.end(); ++it) {
        if (!*it) {
            fprintf(stderr, "%s: prefix not found\n",
                    argv[optind + (it - matched.begin())]);
            status = 1;
        }
    }
    return status;
}

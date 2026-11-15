import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class HeapRuntimeTests(unittest.TestCase):
    def test_sysmem_heap_lifecycle_and_allocator_contract(self):
        self.assertTrue((ROOT / "src" / "heap.c").is_file())
        self.assertTrue((ROOT / "src" / "heap.h").is_file())

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            include = tmp / "include"
            kernel_include = include / "psp2" / "kernel"
            kernel_include.mkdir(parents=True)

            (kernel_include / "sysmem.h").write_text(
                textwrap.dedent(
                    """
                    #pragma once

                    typedef int SceUID;
                    typedef unsigned int SceSize;
                    typedef unsigned int SceKernelMemBlockType;

                    #define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0C20D060U

                    SceUID sceKernelAllocMemBlock(const char *name,
                        SceKernelMemBlockType type, SceSize size, void *opt);
                    int sceKernelGetMemBlockBase(SceUID uid, void **base);
                    int sceKernelFreeMemBlock(SceUID uid);
                    """
                )
            )
            (kernel_include / "clib.h").write_text(
                textwrap.dedent(
                    """
                    #pragma once

                    #include <psp2/kernel/sysmem.h>

                    typedef void *SceClibMspace;

                    SceClibMspace sceClibMspaceCreate(void *base, SceSize size);
                    void sceClibMspaceDestroy(SceClibMspace mspace);
                    void *sceClibMspaceMalloc(SceClibMspace mspace, SceSize size);
                    void *sceClibMspaceCalloc(SceClibMspace mspace,
                        SceSize count, SceSize size);
                    void *sceClibMspaceRealloc(SceClibMspace mspace,
                        void *ptr, SceSize size);
                    void sceClibMspaceFree(SceClibMspace mspace, void *ptr);
                    """
                )
            )
            (kernel_include / "error.h").write_text(
                textwrap.dedent(
                    """
                    #pragma once

                    #define SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE ((int)0x80020009)
                    #define SCE_KERNEL_ERROR_NO_MEMORY ((int)0x8002710B)
                    """
                )
            )

            harness = tmp / "heap_harness.c"
            harness.write_text(
                textwrap.dedent(
                    r"""
                    #include "heap.h"

                    #include <psp2/kernel/clib.h>
                    #include <psp2/kernel/error.h>
                    #include <psp2/kernel/sysmem.h>
                    #include <stdint.h>
                    #include <stdio.h>
                    #include <stdlib.h>
                    #include <string.h>

                    enum {
                        EVENT_ALLOC = 1,
                        EVENT_GET_BASE,
                        EVENT_CREATE,
                        EVENT_DESTROY,
                        EVENT_FREE_MEMBLOCK,
                        EVENT_MALLOC,
                        EVENT_CALLOC,
                        EVENT_REALLOC,
                        EVENT_FREE
                    };

                    static int events[64];
                    static int event_count;
                    static int alloc_call_count;
                    static int get_base_call_count;
                    static int create_call_count;
                    static int destroy_call_count;
                    static int free_memblock_call_count;
                    static int malloc_call_count;
                    static int calloc_call_count;
                    static int realloc_call_count;
                    static int free_call_count;

                    static SceUID alloc_result;
                    static int base_result;
                    static void *mock_base;
                    static SceClibMspace create_result;
                    static void *malloc_result;
                    static void *calloc_result;
                    static void *realloc_result;

                    static const char *last_alloc_name;
                    static SceKernelMemBlockType last_alloc_type;
                    static SceSize last_alloc_size;
                    static SceUID last_base_uid;
                    static void *last_create_base;
                    static SceSize last_create_size;
                    static SceClibMspace last_destroy_space;
                    static SceUID last_freed_uid;
                    static SceClibMspace last_malloc_space;
                    static SceSize last_malloc_size;
                    static SceClibMspace last_calloc_space;
                    static SceSize last_calloc_count;
                    static SceSize last_calloc_size;
                    static SceClibMspace last_realloc_space;
                    static void *last_realloc_ptr;
                    static SceSize last_realloc_size;
                    static SceClibMspace last_free_space;
                    static void *last_free_ptr;

                    static void record_event(int event)
                    {
                        events[event_count++] = event;
                    }

                    static void expect(int condition, int code)
                    {
                        if (condition)
                            return;
                        fprintf(stderr, "heap harness assertion %d failed\n", code);
                        exit(code);
                    }

                    static void reset_mocks(void)
                    {
                        memset(events, 0, sizeof(events));
                        event_count = 0;
                        alloc_call_count = 0;
                        get_base_call_count = 0;
                        create_call_count = 0;
                        destroy_call_count = 0;
                        free_memblock_call_count = 0;
                        malloc_call_count = 0;
                        calloc_call_count = 0;
                        realloc_call_count = 0;
                        free_call_count = 0;

                        alloc_result = 41;
                        base_result = 0;
                        mock_base = (void *)(uintptr_t)0x1000;
                        create_result = (void *)(uintptr_t)0x2000;
                        malloc_result = (void *)(uintptr_t)0x3000;
                        calloc_result = (void *)(uintptr_t)0x4000;
                        realloc_result = (void *)(uintptr_t)0x5000;

                        last_alloc_name = NULL;
                        last_alloc_type = 0;
                        last_alloc_size = 0;
                        last_base_uid = -1;
                        last_create_base = NULL;
                        last_create_size = 0;
                        last_destroy_space = NULL;
                        last_freed_uid = -1;
                        last_malloc_space = NULL;
                        last_malloc_size = 0;
                        last_calloc_space = NULL;
                        last_calloc_count = 0;
                        last_calloc_size = 0;
                        last_realloc_space = NULL;
                        last_realloc_ptr = NULL;
                        last_realloc_size = 0;
                        last_free_space = NULL;
                        last_free_ptr = NULL;
                    }

                    SceUID sceKernelAllocMemBlock(const char *name,
                        SceKernelMemBlockType type, SceSize size, void *opt)
                    {
                        (void)opt;
                        record_event(EVENT_ALLOC);
                        ++alloc_call_count;
                        last_alloc_name = name;
                        last_alloc_type = type;
                        last_alloc_size = size;
                        return alloc_result;
                    }

                    int sceKernelGetMemBlockBase(SceUID uid, void **base)
                    {
                        record_event(EVENT_GET_BASE);
                        ++get_base_call_count;
                        last_base_uid = uid;
                        if (base_result >= 0)
                            *base = mock_base;
                        return base_result;
                    }

                    int sceKernelFreeMemBlock(SceUID uid)
                    {
                        record_event(EVENT_FREE_MEMBLOCK);
                        ++free_memblock_call_count;
                        last_freed_uid = uid;
                        return 0;
                    }

                    SceClibMspace sceClibMspaceCreate(void *base, SceSize size)
                    {
                        record_event(EVENT_CREATE);
                        ++create_call_count;
                        last_create_base = base;
                        last_create_size = size;
                        return create_result;
                    }

                    void sceClibMspaceDestroy(SceClibMspace mspace)
                    {
                        record_event(EVENT_DESTROY);
                        ++destroy_call_count;
                        last_destroy_space = mspace;
                    }

                    void *sceClibMspaceMalloc(SceClibMspace mspace, SceSize size)
                    {
                        record_event(EVENT_MALLOC);
                        ++malloc_call_count;
                        last_malloc_space = mspace;
                        last_malloc_size = size;
                        return malloc_result;
                    }

                    void *sceClibMspaceCalloc(SceClibMspace mspace,
                        SceSize count, SceSize size)
                    {
                        record_event(EVENT_CALLOC);
                        ++calloc_call_count;
                        last_calloc_space = mspace;
                        last_calloc_count = count;
                        last_calloc_size = size;
                        return calloc_result;
                    }

                    void *sceClibMspaceRealloc(SceClibMspace mspace,
                        void *ptr, SceSize size)
                    {
                        record_event(EVENT_REALLOC);
                        ++realloc_call_count;
                        last_realloc_space = mspace;
                        last_realloc_ptr = ptr;
                        last_realloc_size = size;
                        return realloc_result;
                    }

                    void sceClibMspaceFree(SceClibMspace mspace, void *ptr)
                    {
                        record_event(EVENT_FREE);
                        ++free_call_count;
                        last_free_space = mspace;
                        last_free_ptr = ptr;
                    }

                    int main(void)
                    {
                        SceClibMspace active_space;

                        reset_mocks();
                        expect(malloc(1) == NULL, 1);
                        expect(calloc(1, 1) == NULL, 2);
                        expect(realloc(NULL, 1) == NULL, 3);
                        free(NULL);
                        expect(malloc_call_count == 0, 4);
                        expect(calloc_call_count == 0, 5);
                        expect(realloc_call_count == 0, 6);
                        expect(free_call_count == 0, 7);

                        expect(heap_init(0) == SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE, 10);
                        expect(alloc_call_count == 0, 11);
                        expect(heap_init((size_t)-1)
                            == SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE, 12);
                        expect(alloc_call_count == 0, 13);

                        reset_mocks();
                        alloc_result = 41;
                        create_result = (void *)(uintptr_t)0x2100;
                        expect(heap_init(4097) == 0, 20);
                        expect(alloc_call_count == 1, 21);
                        expect(strcmp(last_alloc_name, "vitacompanion_heap") == 0, 22);
                        expect(last_alloc_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 23);
                        expect(last_alloc_size == 8192, 24);
                        expect(get_base_call_count == 1 && last_base_uid == 41, 25);
                        expect(create_call_count == 1, 26);
                        expect(last_create_base == mock_base, 27);
                        expect(last_create_size == 8192, 28);
                        heap_term();
                        expect(event_count == 5, 29);
                        expect(events[0] == EVENT_ALLOC, 30);
                        expect(events[1] == EVENT_GET_BASE, 31);
                        expect(events[2] == EVENT_CREATE, 32);
                        expect(events[3] == EVENT_DESTROY, 33);
                        expect(events[4] == EVENT_FREE_MEMBLOCK, 34);
                        expect(last_destroy_space == (void *)(uintptr_t)0x2100, 35);
                        expect(last_freed_uid == 41, 36);

                        reset_mocks();
                        alloc_result = 51;
                        base_result = -0x123;
                        expect(heap_init(4096) == -0x123, 40);
                        expect(create_call_count == 0, 41);
                        expect(free_memblock_call_count == 1, 42);
                        expect(last_freed_uid == 51, 43);
                        expect(event_count == 3, 44);
                        expect(events[0] == EVENT_ALLOC, 45);
                        expect(events[1] == EVENT_GET_BASE, 46);
                        expect(events[2] == EVENT_FREE_MEMBLOCK, 47);

                        reset_mocks();
                        alloc_result = 52;
                        create_result = NULL;
                        expect(heap_init(4096) == SCE_KERNEL_ERROR_NO_MEMORY, 50);
                        expect(create_call_count == 1, 51);
                        expect(free_memblock_call_count == 1, 52);
                        expect(last_freed_uid == 52, 53);
                        expect(event_count == 4, 54);
                        expect(events[3] == EVENT_FREE_MEMBLOCK, 55);

                        reset_mocks();
                        alloc_result = 53;
                        create_result = (void *)(uintptr_t)0x3100;
                        expect(heap_init(4096) == 0, 60);
                        alloc_result = 54;
                        create_result = (void *)(uintptr_t)0x4100;
                        expect(heap_init(8192) == 0, 61);
                        expect(destroy_call_count == 1, 62);
                        expect(free_memblock_call_count == 1, 63);
                        expect(last_destroy_space == (void *)(uintptr_t)0x3100, 64);
                        expect(last_freed_uid == 53, 65);
                        heap_term();
                        heap_term();
                        expect(destroy_call_count == 2, 66);
                        expect(free_memblock_call_count == 2, 67);
                        expect(last_destroy_space == (void *)(uintptr_t)0x4100, 68);
                        expect(last_freed_uid == 54, 69);

                        reset_mocks();
                        alloc_result = 61;
                        create_result = (void *)(uintptr_t)0x7100;
                        active_space = create_result;
                        expect(heap_init(4096) == 0, 70);
                        reset_mocks();
                        expect(malloc(32) == malloc_result, 71);
                        expect(malloc_call_count == 1, 72);
                        expect(last_malloc_space == active_space, 73);
                        expect(last_malloc_size == 32, 74);
                        expect(calloc(3, 24) == calloc_result, 75);
                        expect(calloc_call_count == 1, 76);
                        expect(last_calloc_space == active_space, 77);
                        expect(last_calloc_count == 3 && last_calloc_size == 24, 78);
                        expect(calloc((size_t)-1, 2) == NULL, 79);
                        expect(calloc_call_count == 1, 80);
                        expect(realloc((void *)(uintptr_t)0x8100, 96)
                            == realloc_result, 81);
                        expect(realloc_call_count == 1, 82);
                        expect(last_realloc_space == active_space, 83);
                        expect(last_realloc_ptr == (void *)(uintptr_t)0x8100, 84);
                        expect(last_realloc_size == 96, 85);
                        free((void *)(uintptr_t)0x9100);
                        expect(free_call_count == 1, 86);
                        expect(last_free_space == active_space, 87);
                        expect(last_free_ptr == (void *)(uintptr_t)0x9100, 88);
                        free(NULL);
                        expect(free_call_count == 2, 89);
                        expect(last_free_ptr == NULL, 90);
                        heap_term();
                        reset_mocks();
                        expect(malloc(1) == NULL, 91);
                        expect(calloc(1, 1) == NULL, 92);
                        expect(realloc(NULL, 1) == NULL, 93);
                        free(NULL);
                        expect(malloc_call_count == 0, 94);
                        expect(calloc_call_count == 0, 95);
                        expect(realloc_call_count == 0, 96);
                        expect(free_call_count == 0, 97);

                        return 0;
                    }
                    """
                )
            )

            executable = tmp / "heap_harness"
            subprocess.run(
                [
                    "cc",
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Dmalloc=vitacompanion_test_malloc",
                    "-Dcalloc=vitacompanion_test_calloc",
                    "-Drealloc=vitacompanion_test_realloc",
                    "-Dfree=vitacompanion_test_free",
                    "-I",
                    str(include),
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "src" / "heap.c"),
                    str(harness),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)


class HeapIntegrationTests(unittest.TestCase):
    def test_user_target_and_lifecycle_use_direct_heap(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        main = (ROOT / "src" / "main.c").read_text()
        heap = (ROOT / "src" / "heap.c").read_text()

        self.assertIn("src/heap.c", cmake)
        self.assertIn("src/heap.h", cmake)
        self.assertIn("SceSysmem_stub", cmake)
        self.assertNotRegex(cmake, r"(?m)^\s*taipool\s*$")
        self.assertIn(
            "#define HEAP_MEMBLOCK_TYPE SCE_KERNEL_MEMBLOCK_TYPE_USER_RW",
            heap,
        )
        self.assertNotIn(
            "defined(SCE_KERNEL_MEMBLOCK_TYPE_USER_RWDATA)",
            heap,
        )

        for legacy in (
            "TAIPOOL_AS_STDLIB",
            "<taipool.h>",
            "taipool_init",
            "taipool_term",
        ):
            self.assertNotIn(legacy, main)

        heap_start = main.index("heap_init(1 * 1024 * 1024)")
        input_start = main.index("input_start()")
        self.assertLess(heap_start, input_start)

        input_failure = re.search(
            r"result = input_start\(\);\s*if \(result < 0\)\s*\{(?P<body>.*?)\n\s*\}",
            main,
            re.DOTALL,
        )
        self.assertIsNotNone(input_failure)
        self.assertIn("heap_term();", input_failure.group("body"))

        nosleep_failure = re.search(
            r"result = nosleep_start\(\);\s*if \(result < 0\)\s*\{(?P<body>.*?)\n\s*\}",
            main,
            re.DOTALL,
        )
        self.assertIsNotNone(nosleep_failure)
        nosleep_body = nosleep_failure.group("body")
        self.assertLess(nosleep_body.index("input_end();"), nosleep_body.index("heap_term();"))

        net_failure = re.search(
            r"result = net_start\(\);\s*if \(result < 0\)\s*\{(?P<body>.*?)\n\s*\}",
            main,
            re.DOTALL,
        )
        self.assertIsNotNone(net_failure)
        net_body = net_failure.group("body")
        self.assertLess(net_body.index("nosleep_end();"), net_body.index("input_end();"))
        self.assertLess(net_body.index("input_end();"), net_body.index("heap_term();"))

        module_stop = main[main.index("module_stop(") :]
        self.assertLess(module_stop.index("net_end();"), module_stop.index("nosleep_end();"))
        self.assertLess(module_stop.index("nosleep_end();"), module_stop.index("input_end();"))
        self.assertLess(module_stop.index("input_end();"), module_stop.index("heap_term();"))

    def test_kernel_driver_boundary_remains_independently_justified(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        reboot = (ROOT / "kernel" / "reboot.c").read_text()
        event_stub = (ROOT / "kernel" / "threadmgr_event_flag_list.S").read_text()

        self.assertIn("__VITA_KERNEL__", cmake)
        self.assertIn("SceThreadmgrForDriver_stub", cmake)
        self.assertIn("SceThreadmgrForDriver_5870C73A", reboot)
        self.assertIn("SceThreadmgrForDriver_5870C73A", event_stub)

    def test_production_sources_have_no_taipool_backend(self):
        files = [ROOT / "CMakeLists.txt", ROOT / "README.md"]
        for directory in ("src", "include", "kernel", "vendor"):
            files.extend(path for path in (ROOT / directory).rglob("*") if path.is_file())

        for path in files:
            source = path.read_text(errors="ignore")
            self.assertNotIn("taipool", source.lower(), path)
            self.assertNotIn("TAIPOOL_AS_STDLIB", source, path)
            self.assertNotIn("POOL_PADDING", source, path)
            self.assertNotIn("mempool_thread", source, path)


if __name__ == "__main__":
    unittest.main()

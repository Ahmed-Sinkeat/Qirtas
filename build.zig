const std = @import("std");

// Build a test executable rooted at src/main.zig with the same C/GTK/sqlite
// linkage as the app, optionally filtered to a subset of tests by name prefix.
// `filters = &.{}` runs everything; `&.{"integration:"}` runs the integration
// suite only. Keeps the three test steps (test / test-integration /
// test-regression) from duplicating the link block.
fn addQirtasTestExe(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    mod: *std.Build.Module,
    gui_files: []const []const u8,
    filters: []const []const u8,
) *std.Build.Step.Compile {
    const t = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "qirtas", .module = mod },
            },
        }),
        .filters = filters,
    });
    t.root_module.linkSystemLibrary("gtk4", .{});
    t.root_module.linkSystemLibrary("gtksourceview-5", .{});
    t.root_module.linkSystemLibrary("libadwaita-1", .{});
    t.root_module.linkSystemLibrary("sqlite3", .{});
    t.root_module.addIncludePath(b.path("src"));
    t.root_module.addCSourceFile(.{ .file = b.path("src/gui.c"), .flags = &.{} });
    t.root_module.addCSourceFile(.{ .file = b.path("src/gui/gui_sync.c"), .flags = &.{} });
    for (gui_files) |file_path| {
        t.root_module.addCSourceFile(.{ .file = b.path(file_path), .flags = &.{} });
    }
    t.root_module.link_libc = true;
    return t;
}

pub fn build(b: *std.Build) void {
    const modular_gui_files = [_][]const u8{
        "src/gui/gui_theme.c",
        "src/gui/gui_cursor.c",
        "src/gui/gui_hr.c",
        "src/gui/gui_codeblock.c",
        "src/gui/gui_table.c",
        "src/gui/gui_search.c",
        "src/gui/gui_conceal.c",
        "src/gui/gui_wiki.c",
        "src/gui/gui_popover.c",
        "src/gui/gui_explorer.c",
        "src/gui/gui_tabs.c",
        "src/gui/gui_editor.c",
        "src/gui/gui_shortcuts.c",
        "src/gui/gui_switcher.c",
        "src/gui/gui_outline.c",
        "src/gui/gui_export.c",
        "src/gui/gui_history.c",
        "src/gui/gui_buffer.c",
        "src/gui/gui_index.c",
        "src/gui/gui_sync_status.c",
        "src/gui/gui_layout.c",
        "src/gui/gui_i18n.c",
        "src/gui/gui_rtl.c",
        "src/gui/gui_dialogs.c",
        "src/gui/gui_statusbar.c",
    };


    // Standard target options allow the person running `zig build` to choose
    // what target to build for. Here we default to native target.
    const target = b.standardTargetOptions(.{});

    // We force standard optimize options but default to ReleaseSmall to satisfy
    // the code footprint budget constraint (<5MB binary size).
    const optimize = b.standardOptimizeOption(.{
        .preferred_optimize_mode = .ReleaseSmall,
    });

    const mod = b.addModule("qirtas", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
    });

    const exe = b.addExecutable(.{
        .name = "qirtas",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "qirtas", .module = mod },
            },
        }),
    });

    // Strip debugging definitions/symbols in release mode to keep binary size low
    if (optimize != .Debug) {
        exe.root_module.strip = true;
    }

    // Link dynamic system dependencies via pkg-config
    exe.root_module.linkSystemLibrary("gtk4", .{});
    exe.root_module.linkSystemLibrary("gtksourceview-5", .{});
    exe.root_module.linkSystemLibrary("libadwaita-1", .{});
    exe.root_module.linkSystemLibrary("sqlite3", .{});
    exe.root_module.addIncludePath(b.path("src"));
    
    // Check lines of modular C files
    inline for (modular_gui_files) |file_path| {
        const file = std.Io.Dir.cwd().openFile(b.graph.io, file_path, .{}) catch |err| {
            std.debug.print("Failed to open {s}: {}\n", .{ file_path, err });
            @panic("File check failed");
        };
        defer file.close(b.graph.io);
        
        var read_buf: [65536]u8 = undefined;
        const bytes_read = file.readPositionalAll(b.graph.io, &read_buf, 0) catch |err| {
            std.debug.print("Failed to read {s}: {}\n", .{ file_path, err });
            @panic("Read failed");
        };
        var line_count: usize = 0;
        for (read_buf[0..bytes_read]) |char| {
            if (char == '\n') {
                line_count += 1;
            }
        }
        if (bytes_read > 0 and read_buf[bytes_read - 1] != '\n') {
            line_count += 1;
        }
        if (line_count > 600) {
            std.debug.print("WARNING: File {s} exceeds 600 lines (actual count: {})\n", .{ file_path, line_count });
        }
    }

    // Add C source wrapper for visual design layers and signals
    exe.root_module.addCSourceFile(.{
        .file = b.path("src/gui.c"),
        .flags = &.{},
    });
    exe.root_module.addCSourceFile(.{
        .file = b.path("src/gui/gui_sync.c"),
        .flags = &.{},
    });
    inline for (modular_gui_files) |file_path| {
        exe.root_module.addCSourceFile(.{
            .file = b.path(file_path),
            .flags = &.{},
        });
    }
    exe.root_module.link_libc = true;

    // Install the executable to the output directory
    b.installArtifact(exe);

    // Build steps to run the application
    const run_step = b.step("run", "Run the application");
    const run_cmd = b.addRunArtifact(exe);
    run_step.dependOn(&run_cmd.step);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    // ── Test suites ──────────────────────────────────────────────────────────
    //   zig build test             → full suite (unit + integration); default.
    //   zig build test-integration → integration round-trips only (name filter).
    //   zig build test-regression  → full suite, the gate CI runs on every push.
    // Integration tests are named "integration: ..."; the regression gate is the
    // whole set, since any test going red is a regression worth blocking on.
    const test_exe = addQirtasTestExe(b, target, optimize, mod, &modular_gui_files, &.{});
    const test_run = b.addRunArtifact(test_exe);
    const test_step = b.step("test", "Run all tests (unit + integration)");
    test_step.dependOn(&test_run.step);

    const itest_exe = addQirtasTestExe(b, target, optimize, mod, &modular_gui_files, &.{"integration:"});
    const itest_run = b.addRunArtifact(itest_exe);
    const itest_step = b.step("test-integration", "Run integration round-trip tests only");
    itest_step.dependOn(&itest_run.step);

    const reg_step = b.step("test-regression", "Full regression gate (all tests) — run in CI");
    reg_step.dependOn(&test_run.step);

    // ── Standalone C behavioral test binary ──────────────────────────────
    // Compiles gui_buffer.c together with tests/test_c_behavior.c (which
    // provides stubs for all Zig symbols). Tests pure C logic — arabize_digits,
    // arabic_count_phrase, arabic_lines_phrase, advance_position — without
    // needing a GTK display. No Zig involved at runtime.
    const c_test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    c_test_mod.addCSourceFile(.{ .file = b.path("tests/test_c_behavior.c"), .flags = &.{} });
    c_test_mod.addCSourceFile(.{ .file = b.path("src/gui/gui_buffer.c"), .flags = &.{} });
    c_test_mod.addIncludePath(b.path("src"));
    c_test_mod.linkSystemLibrary("gtk4", .{});
    c_test_mod.linkSystemLibrary("gtksourceview-5", .{});
    c_test_mod.linkSystemLibrary("libadwaita-1", .{});
    c_test_mod.link_libc = true;

    const c_test_exe = b.addExecutable(.{
        .name = "test-c-behavior",
        .root_module = c_test_mod,
    });
    const c_test_run = b.addRunArtifact(c_test_exe);
    const c_test_step = b.step("test-c", "Run standalone C behavioral tests (no display required)");
    c_test_step.dependOn(&c_test_run.step);
}

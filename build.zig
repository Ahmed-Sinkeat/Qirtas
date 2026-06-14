const std = @import("std");

pub fn build(b: *std.Build) void {
    const modular_gui_files = [_][]const u8{
        "src/gui/gui_theme.c",
        "src/gui/gui_cursor.c",
        "src/gui/gui_hr.c",
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

    const test_exe = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "qirtas", .module = mod },
            },
        }),
    });
    test_exe.root_module.linkSystemLibrary("gtk4", .{});
    test_exe.root_module.linkSystemLibrary("gtksourceview-5", .{});
    test_exe.root_module.linkSystemLibrary("libadwaita-1", .{});
    test_exe.root_module.linkSystemLibrary("sqlite3", .{});
    test_exe.root_module.addIncludePath(b.path("src"));
    test_exe.root_module.addCSourceFile(.{
        .file = b.path("src/gui.c"),
        .flags = &.{},
    });
    test_exe.root_module.addCSourceFile(.{
        .file = b.path("src/gui/gui_sync.c"),
        .flags = &.{},
    });
    inline for (modular_gui_files) |file_path| {
        test_exe.root_module.addCSourceFile(.{
            .file = b.path(file_path),
            .flags = &.{},
        });
    }
    test_exe.root_module.link_libc = true;

    const test_step = b.step("test", "Run tests");
    const test_run = b.addRunArtifact(test_exe);
    test_step.dependOn(&test_run.step);
}

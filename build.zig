const std = @import("std");

pub fn build(b: *std.Build) void {
    // Standard target options allow the person running `zig build` to choose
    // what target to build for. Here we default to native target.
    const target = b.standardTargetOptions(.{});

    // We force standard optimize options but default to ReleaseSmall to satisfy
    // the code footprint budget constraint (<5MB binary size).
    const optimize = b.standardOptimizeOption(.{
        .preferred_optimize_mode = .ReleaseSmall,
    });

    const mod = b.addModule("lawh", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
    });

    const exe = b.addExecutable(.{
        .name = "lawh",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "lawh", .module = mod },
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
    
    // Add C source wrapper for visual design layers and signals
    exe.root_module.addCSourceFile(.{
        .file = b.path("src/gui.c"),
        .flags = &.{},
    });
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
}

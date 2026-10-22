# The machine minikern is built for. The constraint is the project's own: the
# root module does not register @platforms, and a kernel target has nothing in
# common with a host platform anyway (no OS, no libc).
package(default_visibility = ["//:__subpackages__"])

constraint_setting(name = "machine")

# Allwinner H3: quad Cortex-A7 — the NanoPi NEO, and QEMU's orangepi-pc.
constraint_value(
    name = "allwinner_h3",
    constraint_setting = ":machine",
)

platform(
    name = "nanopi_neo",
    constraint_values = [":allwinner_h3"],
)

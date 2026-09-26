/* Mr — runs the Metal 4 milestone inside the app, on the device.
 *
 * The macOS run of this harness reports SKIP, which says nothing about an iPad
 * M-series GPU. What the device needs is the same code the layer is built from,
 * reached from the app rather than from a separate executable: this compiles
 * into the app target and calls the same graphics/metal4 the rest of the
 * project does. There is no second implementation here.
 *
 * Returns a NUL-terminated stage table owned by this file, valid until the next
 * call. The caller logs it; nothing here writes to LogStore, so the two
 * languages meet at one string and no callback is stolen from Wine's logging.
 */
const char *madeira_metal4_selftest(void);

# Default stock-kernel stack for Linux 7.2. Select an older kernel explicitly
# with portable-for-kernel.nix so it retains the required upstream backports.
import ./portable-for-kernel.nix "7.2"

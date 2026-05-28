{ lib
, stdenv
, fetchFromGitHub
, autoreconfHook
, pkg-config
, apple-sdk_26 ? null
, rdma-core-usb4 ? null
, pciutils ? null
, appleCompat ? ./apple-compat
}:

let
  perftest-src = fetchFromGitHub {
    owner = "linux-rdma";
    repo = "perftest";
    rev = "perftest-26.01.5";
    hash = "sha256-WcFOaG5Lzn87EZCZ8w6UTI6qtfX1wOtvF/lxSDwjEq4=";
  };
in
if stdenv.hostPlatform.isDarwin then
  assert lib.assertMsg (apple-sdk_26 != null)
    "perftest Darwin build requires apple-sdk_26 (macOS 26.x SDK exposes /usr/include/infiniband/verbs.h and /usr/lib/librdma.tbd).";
  # Build perftest against the macOS 26 SDK. The SDK exposes the real
  # libibverbs/librdmacm/libibumad headers under usr/include/infiniband and
  # an umbrella `librdma.tbd` at usr/lib/ that re-exports libibverbs.dylib,
  # libibumad.dylib, libmlx5.dylib, libibmad.dylib. We link `-lrdma` and let
  # dyld pick up Apple's RDMA stack at runtime.
  stdenv.mkDerivation {
    pname = "perftest-apple-rdma";
    version = "26.01.5";
    src = perftest-src;

    nativeBuildInputs = [ autoreconfHook ];
    buildInputs = [ apple-sdk_26 ];

    postPatch = ''
      cp -r ${appleCompat} apple-compat
      chmod -R u+w apple-compat
      cp apple-compat/apple_raw_ethernet_stubs.c src/apple_raw_ethernet_stubs.c

      substituteInPlace configure.ac \
        --replace-fail 'AC_CHECK_LIB([ibverbs], [ibv_get_device_list], [], [AC_MSG_ERROR([libibverbs not found])])' 'AC_CHECK_LIB([rdma], [ibv_get_device_list], [], [AC_MSG_ERROR([librdma not found])])' \
        --replace-fail 'AC_CHECK_LIB([rdmacm], [rdma_create_event_channel], [], AC_MSG_ERROR([librdmacm-devel not found]))' 'AC_MSG_NOTICE([librdmacm unavailable on Darwin; rdma_cm mode is not supported])' \
        --replace-fail 'AC_CHECK_LIB([ibumad], [umad_init], [LIBUMAD=-libumad], AC_MSG_ERROR([libibumad not found]))' 'LIBUMAD=' \
        --replace-fail 'AC_CHECK_LIB([ibverbs], [ibv_reg_dmabuf_mr], [HAVE_REG_DMABUF_MR=yes], [HAVE_REG_DMABUF_MR=no])' 'HAVE_REG_DMABUF_MR=no' \
        --replace-fail '[struct ibv_flow *t = ibv_create_flow(NULL,NULL);],[HAVE_RAW_ETH_REG=yes], [HAVE_RAW_ETH_REG=no])' '[struct ibv_flow *t = NULL;],[HAVE_RAW_ETH_REG=no], [HAVE_RAW_ETH_REG=no])' \
        --replace-fail '[int x = IBV_ACCESS_ON_DEMAND;],[HAVE_EX_ODP=yes], [HAVE_EX_ODP=no])' '[int x = IBV_ACCESS_ON_DEMAND;],[HAVE_EX_ODP=no], [HAVE_EX_ODP=no])' \
        --replace-fail 'if [test $IS_FREEBSD = no]; then' 'if false; then' \
        --replace-fail 'AC_CHECK_LIB([dl], [dlclose],' 'AC_CHECK_LIB([System], [printf],' \
        --replace-fail '     LIBS="$LIBS -ldl"],' '     LIBS="$LIBS"],'
      substituteInPlace src/perftest_resources.c \
        --replace-fail 'goto xrc_srq;' 'goto xrcd;'
      # Apple's SDK adds IBV_LINK_LAYER_THUNDERBOLT (=100); upstream perftest
      # treats it as Unknown and aborts. Map it to Ethernet so RoCE-style
      # operation works.
      substituteInPlace src/perftest_parameters.c \
        --replace-fail 'case IBV_LINK_LAYER_ETHERNET:' 'case IBV_LINK_LAYER_ETHERNET: case IBV_LINK_LAYER_THUNDERBOLT:'
      substituteInPlace Makefile.am \
        --replace-fail 'src/host_memory.c src/mmap_memory.c' 'src/host_memory.c src/mmap_memory.c src/apple_raw_ethernet_stubs.c'
    '';

    # apple-compat shim still supplies a few helper headers (byteswap.h etc.)
    # the SDK doesn't ship; the real infiniband/verbs.h comes from the SDK.
    env.NIX_CFLAGS_COMPILE = "-I./apple-compat";
    # -lrdma links Apple's umbrella library so dyld actually loads
    # /usr/lib/librdma.dylib (which re-exports libibverbs.dylib and friends).
    # Apple doesn't ship librdmacm; the unconditional rdma_cm code paths in
    # perftest stay deferred via dynamic_lookup and only blow up if you pass
    # --rdma_cm / -R at runtime.
    env.LDFLAGS = "-lrdma -Wl,-undefined,dynamic_lookup";

    configureFlags = [
      "--disable-cuda"
      "--disable-rocm"
      "--disable-neuron"
      "--disable-ibv_wr_api"
    ];

    meta = with lib; {
      description = "OFED InfiniBand performance tests (linked against macOS 26 SDK librdma)";
      homepage = "https://github.com/linux-rdma/perftest";
      license = licenses.bsd2;
      platforms = platforms.darwin;
    };
  }
else
  # Linux build against our rdma-core-usb4 provider.
  stdenv.mkDerivation {
    pname = "perftest";
    version = "26.01.5";
    src = perftest-src;

    nativeBuildInputs = [ autoreconfHook pkg-config ];
    buildInputs = [ rdma-core-usb4 pciutils ];

    postPatch = ''
      # Force HAVE_MLX5DV=no even though rdma-core ships mlx5dv.h. The
      # perftest negotiate struct grows by 8 bytes when MLX5DV is on, which
      # breaks the on-wire protocol against the Darwin build (Apple's SDK
      # has no mlx5dv.h). Our usb4_rdma devices aren't Mellanox anyway.
      substituteInPlace configure.ac \
        --replace-fail '[HAVE_MLX5DV=yes], [HAVE_MLX5DV=no])' '[HAVE_MLX5DV=no], [HAVE_MLX5DV=no])'
    '';

    configureFlags = [
      "--disable-cuda"
      "--disable-rocm"
      "--disable-neuron"
    ];

    meta = with lib; {
      description = "OFED InfiniBand performance tests (linked against rdma-core-usb4)";
      homepage = "https://github.com/linux-rdma/perftest";
      license = licenses.bsd2;
      platforms = platforms.linux;
    };
  }

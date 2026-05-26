{ lib
, stdenv
, fetchFromGitHub
, autoreconfHook
, pkg-config
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
  # Build perftest on Apple Macs using nixpkgs' Darwin stdenv (no Xcode
  # hardcoded paths, no __noChroot). Headers come from the apple-compat shim
  # that provides Linux-style <infiniband/verbs.h>, <rdma/rdma_cma.h>, etc.;
  # the linker is told to defer libibverbs symbol resolution to dyld via
  # -undefined dynamic_lookup. Apple's RDMA stack provides those symbols at
  # runtime on Macs with USB4-RDMA-capable hardware.
  stdenv.mkDerivation {
    pname = "perftest-apple-rdma";
    version = "26.01.5";
    src = perftest-src;

    nativeBuildInputs = [ autoreconfHook ];

    postPatch = ''
      cp -r ${appleCompat} apple-compat
      chmod -R u+w apple-compat
      cp apple-compat/apple_raw_ethernet_stubs.c src/apple_raw_ethernet_stubs.c

      substituteInPlace configure.ac \
        --replace-fail 'AC_CHECK_LIB([ibverbs], [ibv_get_device_list], [], [AC_MSG_ERROR([libibverbs not found])])' 'AC_MSG_NOTICE([using Apple RDMA verbs via dynamic lookup])' \
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
      substituteInPlace Makefile.am \
        --replace-fail 'src/host_memory.c src/mmap_memory.c' 'src/host_memory.c src/mmap_memory.c src/apple_raw_ethernet_stubs.c'
    '';

    env.NIX_CFLAGS_COMPILE = "-I./apple-compat";
    env.NIX_LDFLAGS = "-Wl,-undefined,dynamic_lookup";

    configureFlags = [
      "--disable-cuda"
      "--disable-rocm"
      "--disable-neuron"
      "--disable-ibv_wr_api"
    ];

    meta = with lib; {
      description = "OFED InfiniBand performance tests (Apple RDMA host SDK build, deferred symbol resolution)";
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

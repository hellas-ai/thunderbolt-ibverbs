{ lib
, stdenv
, fetchFromGitHub
, autoreconfHook
, pkg-config
, rdma-core-usb4
, pciutils
}:

let
  perftest-src = fetchFromGitHub {
    owner = "linux-rdma";
    repo = "perftest";
    rev = "perftest-26.01.5";
    hash = "sha256-WcFOaG5Lzn87EZCZ8w6UTI6qtfX1wOtvF/lxSDwjEq4=";
  };
in
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
    description = "Open Fabrics Enterprise Distribution InfiniBand performance tests (linked against rdma-core-usb4)";
    homepage = "https://github.com/linux-rdma/perftest";
    license = licenses.bsd2;
    platforms = platforms.linux;
  };
}

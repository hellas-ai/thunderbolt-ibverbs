# rdma-core v55+ grew two extra parameters on internal helpers the provider
# uses: ibv_cmd_get_context (new fd_arr parameter at position 4) and
# ibv_cmd_reg_dmabuf_mr (new driver command-buffer parameter at the end).
# Provider source defaults to the v55+ form; here we set guards so the older
# form is used when building against pre-v55 rdma-core (e.g. Ubuntu 22.04
# ships v39, Ubuntu 24.04 ships v50). The exact cutoff is v55.
if(PACKAGE_VERSION VERSION_LESS "55")
    add_definitions(-DUSB4_RDMA_OLD_GET_CONTEXT=1)
    add_definitions(-DUSB4_RDMA_OLD_REG_DMABUF_MR=1)
endif()

rdma_provider(usb4_rdma
  usb4_rdma.c
)

[
  {
    name = "usb4-avoid-reserved-path-fields";
    patch = ./0101-thunderbolt-Avoid-reserved-fields-in-path-config-spa.patch;
  }
  {
    name = "usb4-xdomain-keep-lane-adapter-on-bonding-failure";
    patch = ./0102-thunderbolt-Don-t-disable-lane-adapter-if-XDomain-la.patch;
  }
  {
    name = "usb4-xdomain-usb4v2-lane-bonding";
    patch = ./0103-thunderbolt-Make-XDomain-lane-bonding-comply-with-th.patch;
  }
  {
    name = "usb4-hotplug-keep-domain-ref";
    patch = ./0104-thunderbolt-Keep-the-domain-reference-while-processi.patch;
  }
  {
    name = "usb4-xdomain-release-failed-response-request";
    patch = ./0105-thunderbolt-Release-request-if-tb_cfg_request-fails-.patch;
  }
  {
    name = "usb4-xdomain-clear-root-switch-on-stop";
    patch = ./0106-thunderbolt-Set-tb-root_switch-to-NULL-when-domain-i.patch;
  }
  {
    name = "usb4-domain-wait-release-on-remove";
    patch = ./0107-thunderbolt-Wait-for-tb_domain_release-to-complete-w.patch;
  }
  {
    name = "usb4-xdomain-keep-service-ref";
    patch = ./0108-thunderbolt-Keep-XDomain-reference-during-the-lifeti.patch;
  }
  {
    name = "usb4-dmatest-drop-debugfs-dir-pointer";
    patch = ./0109-thunderbolt-dma_test-No-need-to-store-debugfs-direct.patch;
  }
  {
    name = "usb4-remove-service-debugfs-on-unregister";
    patch = ./0110-thunderbolt-Remove-service-debugfs-entries-during-un.patch;
  }
  {
    name = "usb4-xdomain-remove-without-domain-lock";
    patch = ./0111-thunderbolt-Remove-XDomain-from-the-bus-without-hold.patch;
  }
  {
    name = "usb4-xdomain-no-duplicate-fw-dma-tunnels";
    patch = ./0112-thunderbolt-Don-t-create-multiple-DMA-tunnels-on-fir.patch;
  }
  {
    name = "usb4stream-property-merge-dir";
    patch = ./0113-thunderbolt-Add-tb_property_merge_dir.patch;
  }
  {
    name = "usb4stream-property-merge-dir-kunit";
    patch = ./0114-thunderbolt-Add-KUnit-test-for-tb_property_merge_dir.patch;
  }
  {
    name = "usb4stream-service-driver-properties";
    patch = ./0115-thunderbolt-Allow-service-drivers-to-specify-their-o.patch;
  }
  {
    name = "usb4stream-common-ring-frame-size";
    patch = ./0116-thunderbolt-net-Move-ring_frame_size-to-thunderbolt..patch;
  }
  {
    name = "usb4stream-service-interrupt-throttling";
    patch = ./0117-thunderbolt-net-Let-the-service-drivers-configure-in.patch;
  }
  {
    name = "usb4stream-ring-size-helper";
    patch = ./0118-thunderbolt-Add-helper-to-figure-size-of-the-ring.patch;
  }
  {
    name = "usb4stream-ring-flush";
    patch = ./0119-thunderbolt-Add-tb_ring_flush.patch;
  }
  {
    name = "usb4stream-configfs";
    patch = ./0120-thunderbolt-Add-support-for-ConfigFS.patch;
  }
  {
    name = "usb4stream-driver";
    patch = ./0121-thunderbolt-Add-support-for-USB4STREAM.patch;
  }
]

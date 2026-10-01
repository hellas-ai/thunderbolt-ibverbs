kernelVersion:
if builtins.compareVersions kernelVersion "7.2" < 0 then
  throw "thunderbolt-ibverbs kernel patches require Linux 7.2 or newer; Linux 7.1 backports were retired"
else
  import ./portable.nix

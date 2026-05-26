{ lib
, runCommand
, makeWrapper
, zsh
, gawk
, toolsDir ? ../tools
}:

runCommand "thunderbolt-tools-0.1.0" {
  nativeBuildInputs = [ makeWrapper ];
  meta = with lib; {
    description = "Operator-side Thunderbolt helpers: IRQ row dump and link-speed reporter";
    license = licenses.mit;
    platforms = platforms.linux;
  };
} ''
  mkdir -p $out/bin
  install -m 0755 ${toolsDir}/tb-irq.sh $out/bin/tb-irq
  install -m 0755 ${toolsDir}/tb-speeds.sh $out/bin/tb-speeds
  wrapProgram $out/bin/tb-irq    --prefix PATH : ${lib.makeBinPath [ zsh gawk ]}
  wrapProgram $out/bin/tb-speeds --prefix PATH : ${lib.makeBinPath [ zsh ]}
''

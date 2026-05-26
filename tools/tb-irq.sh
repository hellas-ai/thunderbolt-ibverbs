#!/usr/bin/env zsh
# Show /proc/interrupts rows for thunderbolt with only the nonzero CPU columns,
# preceded by a header listing those CPUs.
awk 'NR==1{for(i=1;i<=NF;i++)h[i]=$i;c=NF;next} /thunder/{a[++n]=$0;for(i=2;i<=c+1;i++)if($i+0)k[i]=1} END{printf"%-5s","IRQ";for(i=2;i<=c+1;i++)if(k[i])printf" %10s",h[i-1];print"  NAME";for(r=1;r<=n;r++){nf=split(a[r],f);printf"%-5s",f[1];for(i=2;i<=c+1;i++)if(k[i])printf" %10s",f[i];printf"  %s %s %s\n",f[nf-2],f[nf-1],f[nf]}}' /proc/interrupts

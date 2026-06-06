# 2026-06-07 Post-Atomic vLLM Smoke

Branch state:

- kernel/module contains the native atomic PSN ordering fix.
- `userspace/bench/tbv_vllm_smoke.sh` resolves the outer USB4 wrapper commands
  to their inner `python3`, `vllm`, and `ray` targets, avoiding the stale RCCL
  `LD_PRELOAD` in the outer wrapper.
- Strix hosts were booted into the refreshed GDA kernel closure (`uname -r`
  `7.0.10` on both hosts).

Command:

```bash
bash userspace/bench/tbv_vllm_smoke.sh \
  --hosts 192.168.23.136,192.168.23.192 \
  --iface eno1 \
  --log-root /mnt/Home/tmp/tbv-vllm-smoke/post-atomic-auto-inner-20260607-011502 \
  --wrapper /nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151 \
  --model /mnt/Home/tmp/tbv-vllm-tiny-qwen3-gda \
  --prepare-tiny-model 1 \
  --rccl-install /mnt/Home/tmp/rccl-hoststream-waitbudget-install \
  --rocshmem-install /mnt/Home/tmp/rocshmem-waitbudget-install \
  --num-prompts 2 --input-len 8 --output-len 4 --max-model-len 512 \
  --kv-cache-bytes 16777216 --tp-size 2 --single-first 1
```

Resolved commands:

```text
python=/nix/store/ckc6lc8s0mx7qfmskdlad3kndlf94ir3-vllm-env-therock-gfx1151/bin/python3
vllm=/nix/store/ckc6lc8s0mx7qfmskdlad3kndlf94ir3-vllm-env-therock-gfx1151/bin/vllm
ray=/nix/store/ckc6lc8s0mx7qfmskdlad3kndlf94ir3-vllm-env-therock-gfx1151/bin/ray
```

Results:

```text
single-node:
  elapsed_time: 0.115649505 s
  requests_per_second: 17.293632169
  tokens_per_second: 207.523586028

Ray TP=2:
  elapsed_time: 2.342705799 s
  requests_per_second: 0.853713685
  tokens_per_second: 10.244564217
```

Counter deltas across the vLLM run:

```text
strix-1:
  dv_poll_wqes +0
  dv_hard_error +0
  data_wr_retransmit +0
  data_wr_timeout +0
  data_wr_retry_exhausted +0
  data_rx_canceled +0
  data_tx_posted/completed +0/+0
  data_tx_errors +0

strix-2:
  dv_poll_wqes +0
  dv_hard_error +0
  data_wr_retransmit +0
  data_wr_timeout +0
  data_wr_retry_exhausted +0
  data_rx_canceled +0
  data_tx_posted/completed +0/+0
  data_tx_errors +0
```

Interpretation:

- The vLLM/Ray lifecycle smoke passes with the rebuilt waitbudget RCCL install
  and no stale RCCL wrapper preload.
- The run does not exercise the GDA verbs data path: TBV counters are unchanged
  and `dv_poll_wqes` does not move. Treat this as an application-environment
  gate, not a GDA throughput benchmark.

Follow-up discriminator:

```bash
bash userspace/bench/tbv_vllm_smoke.sh \
  --hosts 192.168.23.136,192.168.23.192 \
  --iface eno1 \
  --log-root /mnt/Home/tmp/tbv-vllm-smoke/post-atomic-threshold1-20260607-011908 \
  --wrapper /nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151 \
  --model /mnt/Home/tmp/tbv-vllm-tiny-qwen3-gda \
  --prepare-tiny-model 0 \
  --rccl-install /mnt/Home/tmp/rccl-hoststream-waitbudget-install \
  --rocshmem-install /mnt/Home/tmp/rocshmem-waitbudget-install \
  --threshold 1 \
  --num-prompts 2 --input-len 8 --output-len 4 --max-model-len 512 \
  --kv-cache-bytes 16777216 --tp-size 2 --single-first 0
```

Result:

```text
Ray TP=2, RCCL_ROCSHMEM_THRESHOLD=1:
  elapsed_time: 0.286827784 s
  requests_per_second: 6.972825199
  tokens_per_second: 83.673902386
```

TBV counter deltas were still zero on both hosts (`dv_poll_wqes`,
WR retransmits/timeouts, TX posted/completed, TX errors, RX cancels, and atomic
request/response counters). Lowering the threshold does not make this tiny vLLM
path exercise ROCSHMEM/GDA.

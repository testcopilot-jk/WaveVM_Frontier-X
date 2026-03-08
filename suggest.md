{
  "name": "WaveVM-God-Mode",
  "image": "mcr.microsoft.com/devcontainers/base:ubuntu",
  "privileged": true, 
  "capAdd":[
    "NET_ADMIN",
    "SYS_PTRACE",
    "SYS_ADMIN"
  ],
  "securityOpt":[
    "seccomp=unconfined",
    "apparmor=unconfined"
  ],
  "runArgs":[
    "--device=/dev/net/tun:/dev/net/tun"
  ]
}
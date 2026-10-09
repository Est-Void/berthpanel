# berthpanel
Local-first deploy panel for curated self-hosted apps — plus bring-your-own: paste any GitHub repo, get a draft recipe, edit it, deploy. Every deploy gets a sandboxed build and a hardened systemd user unit on an assigned port. No root, allowlisted egress, net.log transparency, and journaled cleanup of interrupted deploys.

## Ports, TLS and the no-root model

berthpanel runs unprivileged: the panel, every deploy and every managed unit are systemd **user** units. That has one honest consequence — a user process cannot bind ports below 1024 on a default host, so terminating TLS on 80/443 needs one of:

- a one-time host setup by you: `sudo sysctl -w net.ipv4.ip_unprivileged_port_start=0` (persist it under `/etc/sysctl.d/`). berthpanel never sets this for you, and it lowers the port floor for every unprivileged process on the host, not only for berthpanel — hence the explicit opt-in; or
- keeping the reverse proxy on a high port and forwarding 80/443 to it from a root-owned system proxy or your router.

There is no mode that is both no-root and able to bind 80/443 without one of those. The panel does not pretend otherwise.

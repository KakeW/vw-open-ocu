# Private ESPHome builds and remote OTA

This runbook targets ESPHome 2026.9.1 on the ESP32-C3 bench gateway. The existing standalone ESP-IDF firmware has no OTA service. A board running standalone firmware needs a full serial install to move to ESPHome. A compatible existing ESPHome OTA installation can be migrated wirelessly. The procedures below require bench validation on this board. [Existing firmware and bench setup](development.md) · [ESPHome 2026.9.1](https://github.com/esphome/esphome/releases/tag/2026.9.1)

## Private external component source

Device Builder fetches external component source while validating and building the YAML. Give that build environment access to the private repository. The ESP32 only receives the compiled firmware. With the component under `components/vw_open_ocu/` at the repository root, the YAML can use the default component path. Replace the example tag with the reviewed revision you deploy. [ESPHome external components](https://esphome.io/components/external_components/)

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/KakeW/vw-open-ocu.git
      ref: bench-release-tag
      username: !secret ocu_git_username
      password: !secret ocu_git_token
    components: [vw_open_ocu]
    refresh: never
```

For GitHub HTTPS, use the GitHub username and a fine-grained token scoped to this repository with read access to Contents. Define `ocu_git_username` and `ocu_git_token` in the builder's `secrets.yaml`. Use a new release tag for each promoted build. `refresh: never` keeps an existing cached checkout, so moving the same tag can make clean builders and cached builders use different source. [GitHub token documentation](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/managing-your-personal-access-tokens) · [Repository permissions](https://docs.github.com/en/codespaces/troubleshooting/troubleshooting-authentication-to-a-repository) · [ESPHome refresh behavior](https://esphome.io/components/external_components/#refresh)

Protect `secrets.yaml`, the builder's `.esphome` directory and any backups containing them. The tagged ESPHome code adds the credentials to the HTTPS clone URL, and the checkout can retain them in Git's remote configuration. When rotating a token for a previously cached source, recreate the affected checkout so subsequent fetches use the new credential. Another option is to check out the reviewed tag separately with the build host's Git credentials and point `external_components` to that checkout as a local source. [Tagged Git implementation](https://github.com/esphome/esphome/blob/2026.9.1/esphome/git.py#L647-L685) · [ESPHome local component sources](https://esphome.io/components/external_components/#local)

For a builder that has the source checked out alongside the device YAML, use a local source. Adjust the path relative to the YAML so it resolves to the directory containing `vw_open_ocu/` inside the build environment. [ESPHome local source paths](https://esphome.io/components/external_components/#local)

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [vw_open_ocu]
```

## First flash and recovery

The native OTA entry requires encryption and inherits the static API key. Use a unique key for this device. The tagged 2026.9.1 schema rejects combining the OTA password with required OTA encryption. [ESPHome 2026.9 OTA release notes](https://esphome.io/blog/2026/09/16/esphome-2026-9/) · [Tagged OTA schema](https://github.com/esphome/esphome/blob/2026.9.1/esphome/components/esphome/ota/__init__.py#L198-L229)

```yaml
api:
  encryption:
    key: !secret ocu_api_key

ota:
  - platform: esphome
    encryption:

safe_mode:
```

1. Use the isolated [bench wiring](hardware.md) and stable power. Save the YAML, secrets, known-good serial-flashable image and, once available, its corresponding OTA image. Keep these in protected storage because the firmware contains credentials. For a board without usable ESPHome OTA, flash the ESPHome factory image over USB from Device Builder. This installs the ESPHome bootloader and partition layout along with the application. If compatible ESPHome OTA is already reachable, follow the [migration steps](esphome.md#configure-the-device) with the installed credentials. [ESPHome getting started](https://esphome.io/guides/getting_started_hassio/) · [OTA encryption and firmware credentials](https://esphome.io/blog/2026/09/16/esphome-2026-9/)
2. Verify boot, Wi-Fi, native API and the gateway's passive bench behavior. The example disables serial application logging. Test one Device Builder wireless update locally and confirm the expected firmware boots, reconnects and captures bench traffic. Retest the same flow from the actual Device Builder host across the VPN before relying on it remotely. The HA API is TCP 6053 by default, and native OTA for ESP32 is a separate listener on TCP 3232. [Native API](https://esphome.io/components/api/#configuration-variables) · [Native OTA](https://esphome.io/components/ota/esphome/#configuration-variables)
3. Verify the component's OTA hooks on the bench. Check that CAN capture pauses during an upload and resumes after an aborted upload or update error. After a successful upload, verify capture starts after reboot. [ESPHome OTA lifecycle callbacks](https://esphome.io/components/ota/#configuration-variables)
4. If the new image repeatedly fails to boot, check for rollback or safe mode. If the encrypted API is still reachable, the example exposes **Reboot into OTA safe mode**. ESP32 rollback is enabled by default and requires a compatible bootloader and an available prior image. With defaults the running image can be confirmed after one minute of uptime or on orderly shutdown. A running image can therefore be confirmed even when capture or HA connectivity is faulty. Test these functions independently after each update. [ESP32 rollback](https://esphome.io/components/esp32/#advanced-options) · [Tagged safe-mode defaults](https://github.com/esphome/esphome/blob/2026.9.1/esphome/components/safe_mode/__init__.py#L31-L44)
5. In safe mode the device keeps networking, OTA and any configured serial logging. If Wi-Fi still works, push the known-good OTA image over the private route. Ordinary API entities and network logging via the API are unavailable in safe mode. If the board has no usable network or OTA listener, recover it locally by serial flashing. A small proof test should exercise the recovery path with the board still on the bench. [ESPHome safe mode](https://esphome.io/components/safe_mode/) · [ESPHome upload and logs commands](https://esphome.io/guides/cli/#upload-command)

## Remote access behind mobile CGNAT

### Device Builder push through a router VPN

Use a stable LAN address for the ESP32 behind the mobile router, for example a DHCP reservation. Route that address from the Device Builder host or container through a VPN terminated on the vehicle router or a dedicated subnet router. Allow TCP 3232 from the build host and TCP 6053 from the intended HA or logging host. Keep those ports off the public internet. Set `wifi.use_address` to a routable IP or private DNS name so Device Builder can upload without relying on `.local` discovery. For a CLI upload use `esphome upload DEVICE.yaml --device ADDRESS`. Check reachability from the build environment itself. The browser's ability to load Device Builder does not establish that route. [ESPHome Wi-Fi addressing](https://esphome.io/components/wifi/#configuration-variables) · [ESPHome CLI upload](https://esphome.io/guides/cli/#upload-command) · [ESPHome security guidance](https://esphome.io/guides/security_best_practices/)

- **Tailscale** can use a subnet router at the vehicle end and a Tailscale client on the build host. A second subnet router can connect the home LAN when the build host and HA run without Tailscale clients. Advertise and approve the vehicle route, grant access to the needed ports and verify return routing. Use distinct subnets for a straightforward site-to-site setup. Tailscale can relay if NAT prevents a direct connection. [Subnet routers](https://tailscale.com/docs/features/subnet-routers) · [Site-to-site](https://tailscale.com/docs/features/site-to-site) · [Direct and relayed connections](https://tailscale.com/docs/reference/device-connectivity)
- **WireGuard** can use the vehicle router as an outbound peer of a home gateway with reachable UDP or a public VPS. Route the vehicle LAN through the peer and make sure forwarding, filtering and the return path work at both ends. When the vehicle peer sits behind NAT, WireGuard documents a 25-second `PersistentKeepalive` as a generally suitable setting to keep a mapping alive. Check the actual router's VPN capabilities before choosing this arrangement. [WireGuard routing model](https://www.wireguard.com/#cryptokey-routing) · [WireGuard keepalive](https://www.wireguard.com/quickstart/#nat-and-firewall-traversal-persistence)

### ESPHome pull over HTTPS

If a router VPN is unavailable and the vehicle can reach a firmware server over HTTPS, an already-provisioned ESPHome device can pull an update. Provision `http_request` and `ota: - platform: http_request` during initial flashing. The `ota.http_request.flash` action fetches an OTA-format image and verifies its expected MD5. A managed `update: - platform: http_request` entity instead polls a manifest for availability and requires an install action to apply the offered image. Pick a trigger that is actually reachable on this deployment. An HA-triggered install still needs a working path for HA to reach the device's API. [ESPHome HTTP OTA](https://esphome.io/components/ota/http_request/) · [Managed HTTP updates](https://esphome.io/components/update/http_request/) · [Native API connection model](https://esphome.io/components/api/)

Publish the Device Builder **OTA format** image through an access-controlled HTTPS endpoint with certificate verification enabled. MD5 checks whether the received image matches the expected digest. Server authentication comes from verified TLS. Keep the build-time Git token off the device and use separate credentials for any artifact access. Direct HTTP flash actions support HTTP Basic authentication if the deployment uses it. Treat a pull-only device as dependent on working networking and its installed pull trigger. To rely on safe-mode pull recovery, test the exact recovery trigger on the bench. [HTTP Request TLS options](https://esphome.io/components/http_request/#configuration-variables) · [HTTP OTA action and authentication](https://esphome.io/components/ota/http_request/#otahttprequestflash-action) · [Safe-mode recovery automation](https://esphome.io/components/safe_mode/#configuration-variables)

Router VPN push is the proposed first path because it gives Device Builder an upload route and can also carry HA API traffic. HTTPS pull is an alternative update delivery path when only outbound connectivity is available. Select the deployment route after verifying the mobile router, build-host connectivity, device wake time and power stability on the bench.

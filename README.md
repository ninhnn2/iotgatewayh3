# iotgatewayh3

### Change systemd to new ttyS* console

- Need to disable sytemd service getty serial0

```shell
sudo systemctl stop serial-getty@ttyS0.service

sudo systemctl disable serial-getty@ttyS0.service

```

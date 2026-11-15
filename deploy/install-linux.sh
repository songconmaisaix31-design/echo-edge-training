set -eu

test "$(id -u)" -eq 0
test -f /opt/echo-edge/src/backend/requirements.txt
test -f /opt/echo-edge/src/frontend/dist/index.html
test -f /opt/echo-edge/models/cardiacudc-fourchamber-v1/CardiacUDC-FourChamber-UNet-epoch18.pt

export DEBIAN_FRONTEND=noninteractive
export TMPDIR=/opt/echo-edge/work
install -d -m 0755 "$TMPDIR" /opt/echo-edge/venvs
apt-get update
apt-get install -y python3-venv
python3 -m venv /opt/echo-edge/venvs/api
python3 -m venv /opt/echo-edge/venvs/model
/opt/echo-edge/venvs/api/bin/python -m pip install --no-cache-dir --index-url https://pypi.org/simple -r /opt/echo-edge/src/backend/requirements.txt
/opt/echo-edge/venvs/model/bin/python -m pip install --no-cache-dir 'torch==2.12.1+cpu' 'torchvision==0.27.1+cpu' --index-url https://download.pytorch.org/whl/cpu
/opt/echo-edge/venvs/model/bin/python -m pip install --no-cache-dir --index-url https://pypi.org/simple -r /opt/echo-edge/src/deploy/model-requirements.txt

if ! getent passwd echo-edge >/dev/null; then
    useradd --system --home-dir /opt/echo-edge --shell /usr/sbin/nologin echo-edge
fi
install -d -o echo-edge -g echo-edge -m 0750 /opt/echo-edge/data /opt/echo-edge/data/media /opt/echo-edge/data/tmp
cd /opt/echo-edge/src/backend
runuser -u echo-edge -- env ECHO_DATABASE_URL=sqlite:////opt/echo-edge/data/platform.db /opt/echo-edge/venvs/api/bin/python -m alembic upgrade head
install -m 0644 /opt/echo-edge/src/deploy/echo-edge.service /etc/systemd/system/echo-edge.service
systemctl daemon-reload

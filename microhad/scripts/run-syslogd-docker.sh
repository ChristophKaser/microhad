#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
container_name=${SYSLOG_CONTAINER_NAME:-microhad-syslogd}
image=${SYSLOG_IMAGE:-balabit/syslog-ng:latest}
log_dir=${SYSLOG_LOG_DIR:-"$project_dir/livelogs"}
config="$script_dir/syslog-ng.conf"

command -v docker >/dev/null 2>&1 || {
    echo "docker is required to run the syslog receiver" >&2
    exit 1
}
mkdir -p "$log_dir"

# Recreate the named container so changes to the mounted config take effect.
if docker container inspect "$container_name" >/dev/null 2>&1; then
    docker rm -f "$container_name" >/dev/null
fi

docker run -d \
    --name "$container_name" \
    --restart unless-stopped \
    -p 7878:7878/udp \
    -v "$config:/etc/syslog-ng/syslog-ng.conf:ro" \
    -v "$log_dir:/var/log/livelogs" \
    "$image" --no-caps

echo "Receiving UDP syslog on port 7878; logs are written to $log_dir/gateway.log"

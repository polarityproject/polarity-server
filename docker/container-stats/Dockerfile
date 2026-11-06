# sfvr-rooms container-stats (container_stats.py): per-container CPU and
# memory from the Docker API. Built by compose.monitoring.yaml.
FROM python:3.12.7-alpine3.20
COPY container_stats.py /usr/local/bin/container_stats.py
ENTRYPOINT ["python3", "-u", "/usr/local/bin/container_stats.py"]

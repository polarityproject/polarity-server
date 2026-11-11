# The rooms publisher (publisher.py): the keeper's public room list to the
# rooms API. Built by compose.monitoring.yaml.
FROM python:3.12.7-alpine3.20
COPY publisher.py /usr/local/bin/publisher.py
USER 65534:10001
ENTRYPOINT ["python3", "-u", "/usr/local/bin/publisher.py"]

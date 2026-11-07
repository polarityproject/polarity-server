# sfvr-reports (deploy/sfvr-reports.py): SFVR games' desync and crash
# reports, received on TCP ${REPORTS_PORT} (5580). Python's standard library
# only. The reports and crashes are the host's folders
# (/var/lib/sfvr-server/reports and crashes, compose.yaml), so they are read,
# copied (scp) and bundled (deploy/collect-reports.py) on the host as before
# the stack; collect-reports.py is here too, for a host without Python.
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --user-group --no-create-home --shell /usr/sbin/nologin sfvr \
 && install -d -o 10001 -g 10001 -m 0750 /reports /crashes
COPY deploy/sfvr-reports.py deploy/collect-reports.py /usr/local/bin/
RUN chmod 0755 /usr/local/bin/sfvr-reports.py /usr/local/bin/collect-reports.py
USER 10001:10001
ENTRYPOINT ["python3", "/usr/local/bin/sfvr-reports.py", "--dir", "/reports", "--crashes-dir", "/crashes"]

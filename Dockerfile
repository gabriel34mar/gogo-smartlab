FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    SMARTLAB_HOST=0.0.0.0 \
    SMARTLAB_PORT=8080 \
    SMARTLAB_DB=/data/smartlab.db

WORKDIR /app
COPY backend/ /app/backend/
COPY frontend/ /app/frontend/

RUN addgroup --system --gid 10001 smartlab \
    && adduser --system --uid 10001 --ingroup smartlab smartlab \
    && mkdir -p /data \
    && chown smartlab:smartlab /data

WORKDIR /app/backend
USER 10001:10001
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/health', timeout=2)" || exit 1

CMD ["python", "server.py"]

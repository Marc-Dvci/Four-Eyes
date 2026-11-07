# The car page in demo mode: no QNX, no keys. The gate's C core is compiled in a build stage and copied in, so the
# running image holds no compiler.
#   docker build -t four-eyes:demo .
#   docker run --rm -p 127.0.0.1:8092:8092 four-eyes:demo      then open http://127.0.0.1:8092

FROM python:3.12-slim-bookworm AS core
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY app/ app/
COPY qnx/core/ qnx/core/
COPY qnx/include/fe_signals.h qnx/include/
RUN python -c "import app.fecore"

FROM python:3.12-slim-bookworm AS demo
ENV PYTHONUNBUFFERED=1 \
    APP_WEB_DIR=/app/web
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app/ app/
COPY web/ web/
COPY qnx/core/ qnx/core/
COPY qnx/include/fe_signals.h qnx/include/
COPY cases/ cases/
COPY evidence/ evidence/
COPY --from=core /app/build/host/ build/host/
RUN useradd --create-home appuser
USER appuser
EXPOSE 8092
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s \
    CMD python -c "import urllib.request as u; u.urlopen('http://127.0.0.1:8092/api/health', timeout=4)"
CMD ["python", "-m", "uvicorn", "app.server:app", "--host", "0.0.0.0", "--port", "8092"]

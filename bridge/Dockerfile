# PineCYD usage bridge, Linux/Docker variant (e.g. a Raspberry Pi) - see README.md.
# Builds on the target machine itself (arm64 on a 64-bit Pi OS, amd64 elsewhere).
FROM python:3.12-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Claude Code CLI - used only for `claude auth login` (one-time, interactive) and to refresh
# that login's token (see refresh_file_credentials() in usage_bridge.py). Installed under
# /root at build time; at runtime HOME points at the /data volume instead, so the login
# itself (~/.claude/.credentials.json, ~/.claude.json) persists across container rebuilds.
RUN curl -fsSL https://claude.ai/install.sh | bash
ENV PATH="/root/.local/bin:${PATH}" \
    PINECYD_CLAUDE_BIN=/root/.local/bin/claude \
    DISABLE_AUTOUPDATER=1 \
    HOME=/data \
    PINECYD_TOKEN_SOURCE=file

COPY usage_bridge.py /app/usage_bridge.py

EXPOSE 8787
CMD ["python3", "-u", "/app/usage_bridge.py"]

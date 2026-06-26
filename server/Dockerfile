# AquaControl backend — Node 22 has the built-in node:sqlite module, so there
# are no native build steps and no system packages to install.
FROM node:22-alpine
WORKDIR /app

# Install deps first for better layer caching.
COPY server/package.json server/package-lock.json* ./server/
RUN cd server && npm install --omit=dev

# Copy the whole repo so the server can also serve index.html / firmware.
COPY . .

ENV PORT=8080
ENV DB_PATH=/data/aquacontrol.db
EXPOSE 8080
WORKDIR /app/server
CMD ["node", "server.js"]

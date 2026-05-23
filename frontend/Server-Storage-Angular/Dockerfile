FROM node:20-alpine AS build

WORKDIR /app
COPY package*.json ./
RUN npm ci

COPY . .
RUN npm run build

FROM nginx:1.27-alpine

COPY nginx-root.conf /etc/nginx/nginx.conf
COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=build /app/dist/server-storage-angular/browser /usr/share/nginx/html

EXPOSE 80
ENTRYPOINT ["nginx"]
CMD ["-g", "daemon off;"]

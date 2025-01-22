FROM ghcr.io/userver-framework/ubuntu-22.04-userver-pg:latest

COPY scripts/docker/setup-dev.sh /userver_tmp/
RUN /userver_tmp/setup-dev.sh && rm -rf /userver_tmp

EXPOSE 8080-8100

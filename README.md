# a simple socks5 implementation

![status](https://github.com/bablon/socks5/actions/workflows/c-cpp.yml/badge.svg)

The default listen port is 9000. Configure the browser with the sock5 forwarding and start this proxy.

## ssl encryption via nginx

With Nginx stream forwarding, this proxy can run on a remote server and receive requests forwarded by Nginx.

### local

ngxsl.conf is the config file for the local Nginx.

### remote server

ngxss.conf is the config file for the remote Nginx, which requires the certificate and key named by cert.pem and key.pem for SSL.

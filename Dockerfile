# 选择一个稳定的基础镜像（和你的宿主机版本接近更好）
FROM ubuntu:22.04

# 避免 apt 的交互式提示
ENV DEBIAN_FRONTEND=noninteractive

# 更新软件源并安装所有编译依赖
RUN 

# 创建工作目录（容器内的挂载点）
WORKDIR /home/mpv

# 默认命令：保持容器运行（用于持久容器）
CMD ["sleep", "infinity"]

.PHONY: docker-build-push-prod docker-build-gvirtus run-gvirtus-backend-dev stop-gvirtus-backend-dev attach-gvirtus-backend-dev-bash run-gvirtus-tests docker-build-openpose run-openpose-test stop-openpose-test docker-build-2d-human-parsing run-2d-human-parsing-test stop-2d-human-parsing-test run-simple-matrix-test

docker-build-push-prod:
	docker buildx build \
		--platform linux/amd64 \
		--push \
		--no-cache \
		-f docker/prod/Dockerfile \
		-t taslanidis/gvirtus:cuda12.6.3-cudnn-ubuntu22.04 \
		.

# Builds a base image.
docker-build-gvirtus:
	docker buildx build \
		--platform linux/amd64 \
		-f docker/dev/Dockerfile \
		-t gvirtus:cuda12.6 \
		.

# Runs the development container with all source code mounted, allowing for fast iteration without rebuilding the image.
run-gvirtus-backend-dev:
	docker run \
		--rm \
		-it \
		--network host \
		--privileged \
		-v ./etc:/opt/GVirtuS/etc/ \
		-v ./include:/opt/GVirtuS/include \
		-v ./plugins:/opt/GVirtuS/plugins \
		-v ./src:/opt/GVirtuS/src \
		-v ./docker/dev/entrypoint.sh:/entrypoint.sh \
		--entrypoint /entrypoint.sh \
		--name gvirtus-backend-dev \
		--runtime=nvidia \
		--shm-size=8G \
		gvirtus:cuda12.6


attach-gvirtus-backend-dev-bash:
		docker exec -it gvirtus-backend-dev bash

run-gvirtus-tests:
	docker exec \
		-it gvirtus-backend-dev \
		bash -c \
		'export LD_LIBRARY_PATH=$$GVIRTUS_HOME/lib/frontend:$$LD_LIBRARY_PATH && \
			cd /gvirtus/build && \
			ctest --output-on-failure'

# OpenPose example.
docker-build-openpose:
	docker buildx build \
		--platform linux/amd64 \
		-f examples/openpose/Dockerfile \
		-t openpose_gvirtus:cuda12.6 \
		examples/openpose

run-openpose-test: 
	docker run --rm \
		--name openpose_test_container \
		--network host \
		-v ./examples/openpose/media:/opt/openpose/examples/media \
		-v ./examples/openpose:/opt/openpose/examples/gvirtus \
		-v ./examples/openpose/properties.json:/opt/GVirtuS/etc/properties.json \
		-v ./examples/openpose/entrypoint.sh:/entrypoint.sh \
		openpose_gvirtus:cuda12.6 \
		bash /entrypoint.sh

# 2D Human Parsing example.
docker-build-2d-human-parsing:
	docker buildx build \
		--platform linux/amd64 \
		-f examples/2d-human-parsing/Dockerfile \
		-t human-parsing_gvirtus:cuda12.6 \
		examples/2d-human-parsing	

run-2d-human-parsing-test: 
	docker run --rm \
		--name human_parsing_test_container \
		--network host \
		--shm-size=8G \
		-v ./include:/opt/GVirtuS/include \
		-v ./plugins:/opt/GVirtuS/plugins \
		-v ./src:/opt/GVirtuS/src \
		-v ./examples/2d-human-parsing/inference_acc_00.py:/opt/2D-Human-Parsing/inference/inference_acc_00.py \
		-v ./examples/2d-human-parsing/demo_imgs:/opt/2D-Human-Parsing/demo_imgs \
		-v ./examples/2d-human-parsing/properties.json:/opt/GVirtuS/etc/properties.json \
		-v ./examples/2d-human-parsing/entrypoint.sh:/entrypoint.sh \
		human-parsing_gvirtus:cuda12.6 \
		bash /entrypoint.sh

# Simple Matrix example.
run-simple-matrix-test:
	docker run \
		--rm \
		-it \
		--name simple_matrix_test_container \
		--network host \
		-v ./include:/opt/GVirtuS/include \
		-v ./plugins:/opt/GVirtuS/plugins \
		-v ./src:/opt/GVirtuS/src \
		-v ./examples/simple_matrix/properties.json:/opt/GVirtuS/etc/properties.json \
		-v ./examples/simple_matrix:/opt/GVirtuS/examples/simple_matrix \
		-v ./examples/simple_matrix/entrypoint.sh:/opt/GVirtuS/entrypoint.sh \
		gvirtus:cuda12.6 \
		bash /opt/GVirtuS/entrypoint.sh

.PHONY: docker-build-push-prod docker-build-gvirtus-dependencies run-gvirtus-backend-dev stop-gvirtus attach-gvirtus-bash run-gvirtus-tests docker-build-gvirtus docker-build-openpose run-openpose-test stop-openpose-test docker-build-2d-human-parsing run-2d-human-parsing-test stop-2d-human-parsing-test run-simple-matrix-test

docker-build-dev-local:
	docker buildx build \
		--platform linux/amd64 \
		--no-cache \
		-f docker/dev/Dockerfile \
		-t gvirtus_backend \
		.

docker-build-push-prod:
	docker buildx build \
		--platform linux/amd64 \
		--push \
		--no-cache \
		-f docker/prod/Dockerfile \
		-t taslanidis/gvirtus:cuda12.6.3-cudnn-ubuntu22.04 \
		.

# For development, we build a base image with all dependencies to speed up iterative testing.
docker-build-gvirtus-dependencies:
	docker buildx build \
		--platform linux/amd64 \
		--no-cache \
		-f docker/dev/Dockerfile \
		-t gvirtus_dependencies:cuda12.6 \
		.

# Runs the development container with all source code mounted, allowing for fast iteration without rebuilding the image.
run-gvirtus-backend-dev:
	docker run \
		--rm \
		-it \
		--network host \
		--privileged \
		-v ./cmake:/gvirtus/cmake/ \
		-v ./etc:/gvirtus/etc/ \
		-v ./include:/gvirtus/include/ \
		-v ./plugins:/gvirtus/plugins/ \
		-v ./src:/gvirtus/src/ \
		-v ./tools:/gvirtus/tools/ \
		-v ./tests:/gvirtus/tests/ \
		-v ./CMakeLists.txt:/gvirtus/CMakeLists.txt \
		-v ./docker/dev/entrypoint.sh:/entrypoint.sh \
		-v ./examples:/gvirtus/examples/ \
		--entrypoint /entrypoint.sh \
		--name gvirtus \
		--runtime=nvidia \
		--shm-size=8G \
		gvirtus_dependencies:cuda12.6

stop-gvirtus:
	docker stop gvirtus || true

run-gvirtus-backend-dev-local:
	docker run \
		--rm \
		-it \
		--network host \
		--privileged \
		-v ./cmake:/gvirtus/cmake/ \
		-v ./etc:/gvirtus/etc/ \
		-v ./include:/gvirtus/include/ \
		-v ./plugins:/gvirtus/plugins/ \
		-v ./src:/gvirtus/src/ \
		-v ./tools:/gvirtus/tools/ \
		-v ./tests:/gvirtus/tests/ \
		-v ./CMakeLists.txt:/gvirtus/CMakeLists.txt \
		-v ./docker/dev/entrypoint.sh:/entrypoint.sh \
		-v ./examples:/gvirtus/examples/ \
		--entrypoint /entrypoint.sh \
		--name gvirtus \
		--runtime=nvidia \
		--shm-size=8G \
		gvirtus_backend

attach-gvirtus-bash:
		docker exec -it gvirtus bash

run-gvirtus-tests:
	docker exec \
		-it gvirtus \
		bash -c \
		'export LD_LIBRARY_PATH=$$GVIRTUS_HOME/lib/frontend:$$LD_LIBRARY_PATH && \
			cd /gvirtus/build && \
			ctest --output-on-failure'

# Builds a base image used for frontend examples.
docker-build-gvirtus:
	docker buildx build \
		--platform linux/amd64 \
		-f docker/dev/Dockerfile.frontend \
		-t gvirtus:cuda12.6 \
		.

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

docker-build-matrix-mul-test-local:
	docker buildx build \
		--platform linux/amd64 \
		-t matrix-mul \
		-f ./examples/simple_matrix_local/Dockerfile-local \
		.

run-matrix-mul-test-local: 
	docker run \
		--rm \
		-it \
		--network host \
		--privileged \
		-v ./cmake:/gvirtus/cmake/ \
		-v ./etc:/gvirtus/etc/ \
		-v ./include:/gvirtus/include/ \
		-v ./plugins:/gvirtus/plugins/ \
		-v ./src:/gvirtus/src/ \
		-v ./tools:/gvirtus/tools/ \
		-v ./tests:/gvirtus/tests/ \
		-v ./CMakeLists.txt:/gvirtus/CMakeLists.txt \
		-v ./examples:/gvirtus/examples/ \
		matrix-mul \
		bash /entrypoint.sh

stop-openpose-test:
	docker stop openpose_test_container || true

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
		-v ./examples/2d-human-parsing/inference_acc_00.py:/opt/2D-Human-Parsing/inference/inference_acc_00.py \
		-v ./examples/2d-human-parsing/demo_imgs:/opt/2D-Human-Parsing/demo_imgs \
		-v ./examples/2d-human-parsing/properties.json:/opt/GVirtuS/etc/properties.json \
		-v ./examples/2d-human-parsing/entrypoint.sh:/entrypoint.sh \
		human-parsing_gvirtus:cuda12.6 \
		bash /entrypoint.sh

stop-2d-human-parsing-test:
	docker stop human_parsing_test_container || true

# Simple Matrix example.
run-simple-matrix-test:
	docker run \
		--rm \
		-it \
		--name simple_matrix_test_container \
		--network host \
		-v ./examples/simple_matrix/properties.json:/opt/GVirtuS/etc/properties.json \
		-v ./examples/simple_matrix:/opt/GVirtuS/examples/simple_matrix \
		-v ./examples/simple_matrix/frontend.sh:/opt/GVirtuS/frontend.sh \
		gvirtus:cuda12.6 \
		bash /opt/GVirtuS/frontend.sh

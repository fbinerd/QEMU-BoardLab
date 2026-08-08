IMAGE := arm-selfmod-lab:latest

.PHONY: build run-01 run-02 shell

build:
	docker build -t $(IMAGE) .

run-01: build
	docker run --rm -v "$(CURDIR):/src" $(IMAGE) \
		bash -c "cd /src && ./build.sh 01-qemu-self-mod-limitation && ./qemu-run.sh 01-qemu-self-mod-limitation"

run-02: build
	docker run --rm -v "$(CURDIR):/src" $(IMAGE) \
		bash -c "cd /src && ./build.sh 02-stack-redirect-poc && ./qemu-run.sh 02-stack-redirect-poc"

shell: build
	docker run --rm -it -v "$(CURDIR):/src" $(IMAGE) bash

# ----- BUILD ----- 
FROM ubuntu:latest AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    g++ \
    make

WORKDIR /app

COPY . .

RUN make

# ----- RUNTIME ----- 
FROM ubuntu:latest 

WORKDIR /app

COPY --from=builder /app/build/server . 

EXPOSE 8080

CMD ["./server"]


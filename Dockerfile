# Same environment as the course grader.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ make zip && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir z3-solver==5.3.1
WORKDIR /lab
COPY . /lab
CMD ["sh", "-c", "make && make grade"]

package main

import "fmt"

func ponger(ping chan int64, pong chan int64) {
	for {
		v := <-ping
		if v == -1 {
			return
		}
		pong <- v + 1
	}
}

func main() {
	ping := make(chan int64, 1)
	pong := make(chan int64, 1)
	go ponger(ping, pong)
	var total int64 = 0
	for i := int64(0); i < 100000; i++ {
		ping <- i
		total += <-pong
	}
	ping <- -1
	fmt.Println(total)
}

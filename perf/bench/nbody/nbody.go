// IDIOMATIC-CLASS: normal Go — pointers into the array, mutation in place.
package main

import "fmt"

type P struct {
	x, y, vx, vy int64
}

func interact(a *P, b *P) {
	dx := b.x - a.x
	dy := b.y - a.y
	a.vx += dx / 16
	a.vy += dy / 16
}

func main() {
	var ps [8]P
	for i := 0; i < 8; i++ {
		ps[i].x = int64(i) * 100
		ps[i].y = int64(i) * 37
		ps[i].vx = 1
		ps[i].vy = 2
	}
	for s := 0; s < 2000000; s++ {
		for i := 0; i < 8; i++ {
			for j := 0; j < 8; j++ {
				if i != j {
					interact(&ps[i], &ps[j])
				}
			}
		}
		for i := 0; i < 8; i++ {
			ps[i].x += ps[i].vx
			ps[i].y += ps[i].vy
		}
	}
	var h int64 = 0
	for i := 0; i < 8; i++ {
		h += ps[i].x + ps[i].y + ps[i].vx + ps[i].vy
	}
	fmt.Println(h)
}

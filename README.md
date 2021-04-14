# dtao - dzen for Wayland

(zen + way -> tao)

This is (will be) a stdin-based general-purpose bar for Wayland, modeled after the venerable [dzen2](https://github.com/robm/dzen).  At the moment, it does little more than display text from stdin on a layer-shell, but the tricky Wayland stuff is out of the way, so it's ready for features to be ported from dzen2.


## Dependencies

* [fcft](https://codeberg.org/dnkl/fcft)
* pixman
* libwayland-client
* ruby-ronn


## To-do list/contribution opportunities

* Colors with ^fg() and ^bg()
* Cleanup (currently a cobbled-together mess of examples)

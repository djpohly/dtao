# dtao - dzen for Wayland

(zen + way -> tao)

dtao is a stdin-based general-purpose bar for Wayland, modeled after the venerable [dzen2](https://github.com/robm/dzen).  At the moment, it can display text from stdin on a layer-shell and change foreground and background colors, but at this point it should be easy to port more features from dzen without needing too much Wayland knowhow.


## Dependencies

* [fcft](https://codeberg.org/dnkl/fcft)
* pixman
* libwayland-client
* ruby-ronn


## To-do list/contribution opportunities

* Port other formatting commands from dzen
* Continued refactoring and cleanup

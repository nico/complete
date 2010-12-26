goog.provide('complete')

goog.require('goog.dom');
goog.require('goog.ui.AutoComplete.Remote');

complete.setUp = function() {
  var input = goog.dom.getElement('txtInput');
  // TODO:
  // http://closure-library.googlecode.com/svn/trunk/closure/goog/demos/autocompleterichremote.html
  // (view source; use rich results.)
  // FIXME: be disposable
  var ac = new goog.ui.AutoComplete.Remote('http://hummer.mtv:8080', input);

  // FIXME: would be nice to have the box open immediately.
  //setTimeout(function() {
    ////ac.setActiveElement(ac.getInputHandler().getActiveElement());  // null??
    //ac.setToken(' ');

    ////ac.getInputHandler().update(true);
  //}, 800);
}

complete.setUp();

goog.exportSymbol('complete.setUp', complete.setUp);

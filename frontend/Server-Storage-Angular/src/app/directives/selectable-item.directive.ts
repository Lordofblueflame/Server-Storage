import { Directive, Input, HostListener, ElementRef } from '@angular/core';
import { SelectionService } from '../services/selection.service';

@Directive({
  standalone: true,
  selector: '[appSelectableItem]'
})
export class SelectableItemDirective {
  @Input() index!: number;

  constructor(private selectionService: SelectionService, private el: ElementRef) {}

  @HostListener('mousedown', ['$event'])
  onMouseDown(event: MouseEvent) {
    if (event.shiftKey && this.selectionService.lastSelectedIndex !== null) {
      this.selectionService.selectRange(this.selectionService.lastSelectedIndex, this.index);
      return;
    }

    if (event.ctrlKey || event.metaKey) {
      this.selectionService.toggleSelection(this.index);
      return;
    }

    this.selectionService.clearAndSelect(this.index);
  }

  @HostListener('mouseover', ['$event'])
  onMouseOver(event: MouseEvent) {
    if (event.buttons === 1 && this.selectionService.lastSelectedIndex !== null) {
      this.selectionService.selectedItems.add(this.index);
    }
  }
}
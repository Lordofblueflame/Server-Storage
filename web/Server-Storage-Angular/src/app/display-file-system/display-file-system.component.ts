import { Component, Input } from '@angular/core';
import { CommonModule } from '@angular/common';
import { DirectoryViewComponent } from '../directory-view/directory-view.component';
import { FileViewComponent } from '../file-view/file-view.component';
import { DisplayFormat } from '../enums/display-format';
import { SelectionService } from '../services/selection.service';
import { SelectableItemDirective } from '../directives/selectable-item.directive';

@Component({
  selector: 'app-display-file-system',
  standalone: true,
  imports: [CommonModule, DirectoryViewComponent, FileViewComponent, SelectableItemDirective],
  templateUrl: './display-file-system.component.html',
  styleUrl: './display-file-system.component.css'
})


export class DisplayFileSystemComponent {
  @Input() currentDirectory: string = "X:\\";
  @Input() currentDirectories: Array<{ path: string }> = [];
  @Input() currentFiles: Array<{ path: string, last_write_time: string, file_size: number }> = [];
  @Input() navigateToSubdirectory!: (subdirectoryPath: string) => void;

  currentDisplayFormat: DisplayFormat = DisplayFormat.Details;
  selectedItems = new Set<number>();
  lastSelectedIndex: number | null = null; 
  constructor(private selectionService: SelectionService) {}

  isDetails(): boolean {
    return this.currentDisplayFormat === DisplayFormat.Details;
  }

  getDisplayFormatClass(): string {
    switch (this.currentDisplayFormat) {
      case DisplayFormat.List:
        return 'format-list';
      case DisplayFormat.Details:
        return 'format-details';
      case DisplayFormat.Grid:
        return 'format-grid';
      case DisplayFormat.Tiles:
        return 'format-tiles';
      default:
        return 'format-grid';
    }
  }

  trackByIndex(index: number, item: any) {
    return index;

  }
  
  onMouseDown(event: MouseEvent, index: number) {
    if (event.shiftKey && this.selectionService.lastSelectedIndex !== null) {
      this.selectionService.selectRange(this.selectionService.lastSelectedIndex, index);
      return;
    }

    if (event.ctrlKey || event.metaKey) {
      this.selectionService.toggleSelection(index);
      return;
    }

    this.selectionService.clearAndSelect(index);
  }

  onMouseOver(event: MouseEvent, index: number) {
    if (event.buttons === 1 && this.selectionService.lastSelectedIndex !== null) {
      this.selectionService.selectedItems.add(index);
    }
  }

  isSelected(index: number): boolean {
    return this.selectionService.isSelected(index);
  }
}

